#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>
#include <sqlite3.h>
#include <sstream>
#include <iostream>

#include <Events/PlayerLeaveCellEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetCellComponent().Cell == acEvent.OldCell)
            return;
    }

    auto objectView = m_world.view<ObjectComponent, CellIdComponent>();
    Vector<entt::entity> toDestroy;

    for (auto entity : objectView)
    {
        const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);

        if (cellIdComponent.Cell != acEvent.OldCell)
            continue;

        toDestroy.push_back(entity);
    }

    for (auto& entity : toDestroy)
    {
        m_world.destroy(entity);
    }
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;

            objectData.IsSenderFirst = false;

            response.Objects.push_back(objectData);
            
            // Ensure door state is synced if it is open
            if (objectComponent.CurrentOpenState) {
                 NotifyActivate notify;
                 notify.Id = object.Id;
                 notify.ActivatorId = 0;
                 notify.PreActivationOpenState = 0;
                 acMessage.pPlayer->Send(notify);
            }
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.pPlayer);
            objectComponent.CurrentLockData = object.CurrentLockData;

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = object.CurrentInventory;

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = true;

            // Database persistence logic
            sqlite3* db = GameServer::Get()->GetDB();
            if (db) {
                std::string sql = fmt::format("SELECT is_locked, lock_level, open_state FROM Objects WHERE form_id = {};", object.Id);
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        objectComponent.CurrentLockData.IsLocked = sqlite3_column_int(stmt, 0);
                        objectComponent.CurrentLockData.LockLevel = sqlite3_column_int(stmt, 1);
                        objectComponent.CurrentOpenState = sqlite3_column_int(stmt, 2);
                        
                        // Assume authoritative state
                        objectData.IsSenderFirst = false; 
                        objectData.CurrentLockData.IsLocked = objectComponent.CurrentLockData.IsLocked;
                        objectData.CurrentLockData.LockLevel = objectComponent.CurrentLockData.LockLevel;
                        
                        if (objectComponent.CurrentOpenState) {
                             NotifyActivate notify;
                             notify.Id = object.Id;
                             notify.ActivatorId = 0;
                             notify.PreActivationOpenState = 0;
                             acMessage.pPlayer->Send(notify);
                        }
                    } else {
                        // Does not exist, create the entry with client's reported state
                        std::string ins_sql = fmt::format(
                            "INSERT INTO Objects (form_id, is_locked, lock_level, open_state, inventory) VALUES ({}, {}, {}, 0, '[]');",
                            object.Id, object.CurrentLockData.IsLocked, object.CurrentLockData.LockLevel
                        );
                        char* zErrMsg = 0;
                        sqlite3_exec(db, ins_sql.c_str(), 0, 0, &zErrMsg);
                        if (zErrMsg) sqlite3_free(zErrMsg);
                    }
                    sqlite3_finalize(stmt);
                }
            }

            response.Objects.push_back(objectData);
        }
    }

    if (!response.Objects.empty())
        acMessage.pPlayer->Send(response);
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();
    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentOpenState = !acMessage.Packet.PreActivationOpenState;

        sqlite3* db = GameServer::Get()->GetDB();
        if (db) {
            std::string sql = fmt::format(
                "INSERT INTO Objects (form_id, is_locked, lock_level, open_state, inventory) VALUES ({}, 0, 0, {}, '[]') "
                "ON CONFLICT(form_id) DO UPDATE SET open_state=excluded.open_state;",
                acMessage.Packet.Id, objectComponent.CurrentOpenState
            );
            char* zErrMsg = 0;
            sqlite3_exec(db, sql.c_str(), 0, 0, &zErrMsg);
            if (zErrMsg) sqlite3_free(zErrMsg);
        }
    }

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
        {
            pPlayer->Send(notifyActivate);
        }
    }
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;

        sqlite3* db = GameServer::Get()->GetDB();
        if (db) {
            std::string sql = fmt::format(
                "INSERT INTO Objects (form_id, is_locked, lock_level, open_state, inventory) VALUES ({}, {}, {}, 0, '[]') "
                "ON CONFLICT(form_id) DO UPDATE SET is_locked=excluded.is_locked, lock_level=excluded.lock_level;",
                acMessage.Packet.Id, acMessage.Packet.IsLocked, acMessage.Packet.LockLevel
            );
            char* zErrMsg = 0;
            sqlite3_exec(db, sql.c_str(), 0, 0, &zErrMsg);
            if (zErrMsg) sqlite3_free(zErrMsg);
        }
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer == acMessage.pPlayer)
            continue;

        if (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId)
            pPlayer->Send(notifyLockChange);
    }
}

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    auto& packet = acMessage.Packet;

    NotifyScriptAnimation message{};
    message.FormID = packet.FormID;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        pPlayer->Send(message);
    }
}
