#include "InventoryService.h"

#include <Components.h>
#include <World.h>
#include <GameServer.h>
#include <sqlite3.h>
#include <sstream>

#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>

#include <Setting.h>
namespace
{
Console::Setting bEnableItemDrops{"Gameplay:bEnableItemDrops", "(Experimental) Syncs dropped items by players", false};
}

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_inventoryChangeConnection = aDispatcher.sink<PacketEvent<RequestInventoryChanges>>().connect<&InventoryService::OnInventoryChanges>(this);
    m_equipmentChangeConnection = aDispatcher.sink<PacketEvent<RequestEquipmentChanges>>().connect<&InventoryService::OnEquipmentChanges>(this);
    m_drawWeaponConnection = aDispatcher.sink<PacketEvent<DrawWeaponRequest>>().connect<&InventoryService::OnWeaponDrawnRequest>(this);
}

void InventoryService::OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it != view.end())
    {
        auto& inventoryComponent = view.get<InventoryComponent>(*it);

        if (m_world.try_get<OwnerComponent>(*it) && acMessage.pPlayer) {
            if (message.Item.Count < 0) {
                int32_t currentCount = 0;
                for (const auto& e : inventoryComponent.Content.Entries) {
                    if (e.BaseId == message.Item.BaseId) {
                        currentCount = e.Count;
                        break;
                    }
                }
                
                if (currentCount < std::abs(message.Item.Count)) {
                    spdlog::warn("Inventory authority rejected drop from {}: attempt {:x} count {}", acMessage.pPlayer->GetUsername().c_str(), message.Item.BaseId, std::abs(message.Item.Count));
                    
                    NotifyInventoryChanges revert;
                    revert.ServerId = message.ServerId;
                    revert.Item = message.Item;
                    revert.Item.Count = std::abs(message.Item.Count); 
                    revert.Drop = false;
                    acMessage.pPlayer->Send(revert);
                    return; 
                }
            }
        }

        inventoryComponent.Content.AddOrRemoveEntry(message.Item);

        // Database persistence logic for objects
        if (m_world.try_get<FormIdComponent>(*it)) {
            sqlite3* db = GameServer::Get()->GetDB();
            if (db) {
                auto& formIdComponent = m_world.get<FormIdComponent>(*it);
                
                std::stringstream invJson;
                invJson << "[";
                for (size_t i = 0; i < inventoryComponent.Content.Entries.size(); ++i) {
                    auto& e = inventoryComponent.Content.Entries[i];
                    invJson << "{\"BaseId\":" << e.BaseId << ",\"Count\":" << e.Count << ",\"Worn\":" << (e.IsWorn() ? "true" : "false") << "}";
                    if (i < inventoryComponent.Content.Entries.size() - 1) invJson << ",";
                }
                invJson << "]";
                
                std::string sql = fmt::format(
                    "UPDATE Objects SET inventory = '{}' WHERE form_id = {};",
                    invJson.str(), formIdComponent.Id
                );
                
                char* zErrMsg = 0;
                sqlite3_exec(db, sql.c_str(), 0, 0, &zErrMsg);
                if (zErrMsg) sqlite3_free(zErrMsg);
            }
        }
    }

    if (!message.UpdateClients)
        return;

    NotifyInventoryChanges notify;
    notify.ServerId = message.ServerId;
    notify.Item = message.Item;

    notify.Drop = bEnableItemDrops ? message.Drop : false;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it != view.end())
    {
        auto& inventoryComponent = view.get<InventoryComponent>(*it);
        inventoryComponent.Content.UpdateEquipment(message.CurrentInventory);
    }

    NotifyEquipmentChanges notify;
    notify.ServerId = message.ServerId;
    notify.ItemId = message.ItemId;
    notify.EquipSlotId = message.EquipSlotId;
    notify.Count = message.Count;
    notify.Unequip = message.Unequip;
    notify.IsSpell = message.IsSpell;
    notify.IsShout = message.IsShout;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != std::end(characterView) && characterView.get<OwnerComponent>(*it).GetOwner() == acMessage.pPlayer)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetWeaponDrawn(message.IsWeaponDrawn);
        spdlog::debug("Updating weapon drawn state {:x}:{}", message.Id, message.IsWeaponDrawn);
    }
}
