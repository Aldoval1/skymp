#include <GameServer.h>
#include <glm/glm.hpp>

#include <Services/OverlayService.h>

#include <ChatMessageTypes.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/TeleportRequest.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include "Game/Player.h"

#include <regex>


OverlayService::OverlayService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_chatMessageConnection = aDispatcher.sink<PacketEvent<SendChatMessageRequest>>().connect<&OverlayService::HandleChatMessage>(this);
    m_playerDialogueConnection = aDispatcher.sink<PacketEvent<PlayerDialogueRequest>>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportRequest>>().connect<&OverlayService::OnTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<PacketEvent<RequestPlayerHealthUpdate>>().connect<&OverlayService::OnPlayerHealthUpdate>(this);
}

void sendPlayerMessage(const ChatMessageType acType, const String acContent, Player* aSendingPlayer) noexcept
{
    NotifyChatMessageBroadcast notifyMessage{};

    std::regex escapeHtml{"<[^>]+>\\s+(?=<)|<[^>]+>"};
    notifyMessage.MessageType = acType;
    notifyMessage.PlayerName = std::regex_replace(aSendingPlayer->GetUsername(), escapeHtml, "");
    notifyMessage.ChatMessage = std::regex_replace(acContent, escapeHtml, "");

    auto character = aSendingPlayer->GetCharacter();

    switch (notifyMessage.MessageType)
    {
    case kGlobalChat: 
    {
        if (character) {
             auto& world = GameServer::Get()->GetWorld();
             const auto* senderMove = world.try_get<MovementComponent>(*character);
             const auto& senderCell = aSendingPlayer->GetCellComponent();

             for (Player* p : world.GetPlayerManager()) {
                 if (p->GetCellComponent().Cell == senderCell.Cell) {
                     auto pChar = p->GetCharacter();
                     if (pChar) {
                         const auto* targetMove = world.try_get<MovementComponent>(*pChar);
                         if (senderMove && targetMove) {
                             float dist = glm::distance(senderMove->Position, targetMove->Position);
                             if (dist <= 3000.f) { 
                                 p->Send(notifyMessage);
                             }
                         } else {
                             p->Send(notifyMessage);
                         }
                     }
                 }
             }
        } else {
             GameServer::Get()->SendToPlayers(notifyMessage);
        }
        break;
    }

    case kSystemMessage: spdlog::error("PlayerId {} attempted to send a System Message.", aSendingPlayer->GetId()); break;

    case kPlayerDialogue: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kPartyChat: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kLocalChat:
        if (character)
        {
            if (!GameServer::Get()->SendToPlayersInRange(notifyMessage, *character))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
        }
        break;
    default: spdlog::error("{} is not a known MessageType", static_cast<uint64_t>(notifyMessage.MessageType)); break;
    }
}

void OverlayService::HandleChatMessage(const PacketEvent<SendChatMessageRequest>& acMessage) const noexcept
{
    auto [canceled, reason] = m_world.GetScriptService().HandleChatMessage(*acMessage.pPlayer->GetCharacter(), acMessage.Packet.ChatMessage);
    if (canceled)
        return;

    std::string msgTemp(acMessage.Packet.ChatMessage.c_str());

    if (msgTemp.starts_with("/private ")) {
        size_t firstSpace = msgTemp.find(' ', 9);
        if (firstSpace != std::string::npos) {
            std::string targetName = msgTemp.substr(9, firstSpace - 9);
            std::string actualMessage = msgTemp.substr(firstSpace + 1);
            
            Player* targetPlayer = nullptr;
            for (Player* p : m_world.GetPlayerManager()) {
                if (p->GetUsername() == targetName) {
                    targetPlayer = p;
                    break;
                }
            }
            if (targetPlayer) {
                NotifyChatMessageBroadcast notify;
                notify.MessageType = kSystemMessage; 
                notify.PlayerName = "[Private] " + std::string(acMessage.pPlayer->GetUsername().c_str());
                notify.ChatMessage = actualMessage;
                targetPlayer->Send(notify);
                
                notify.PlayerName = "[Private to " + targetName + "]";
                acMessage.pPlayer->Send(notify);
            } else {
                NotifyChatMessageBroadcast notFound;
                notFound.MessageType = kSystemMessage;
                notFound.PlayerName = "System";
                notFound.ChatMessage = "Player not found.";
                acMessage.pPlayer->Send(notFound);
            }
        }
        return;
    }

    if (msgTemp.starts_with("/f ")) {
        std::string actualMessage = msgTemp.substr(3);
        std::string faction = acMessage.pPlayer->GetFaction().c_str();
        if (faction.empty()) {
            NotifyChatMessageBroadcast noFac;
            noFac.MessageType = kSystemMessage;
            noFac.PlayerName = "System";
            noFac.ChatMessage = "You are not in a faction.";
            acMessage.pPlayer->Send(noFac);
            return;
        }
        
        NotifyChatMessageBroadcast notify;
        notify.MessageType = kGlobalChat; 
        notify.PlayerName = "[Faction] " + std::string(acMessage.pPlayer->GetUsername().c_str());
        notify.ChatMessage = actualMessage;
        
        for (Player* p : m_world.GetPlayerManager()) {
            if (p->GetFaction() == faction) {
                p->Send(notify);
            }
        }
        return;
    }

    sendPlayerMessage(acMessage.Packet.MessageType, acMessage.Packet.ChatMessage, acMessage.pPlayer);
}

void OverlayService::OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) const noexcept
{
    sendPlayerMessage(kPlayerDialogue, acMessage.Packet.Text, acMessage.pPlayer);
}

void OverlayService::OnTeleport(const PacketEvent<TeleportRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = m_world.GetPlayerManager().GetById(acMessage.Packet.PlayerId);
    if (!pTargetPlayer)
        return;

    NotifyTeleport response{};

    auto character = pTargetPlayer->GetCharacter();
    if (character)
    {
        const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
        if (pMovementComponent)
        {
            const auto& cellComponent = pTargetPlayer->GetCellComponent();
            response.CellId = cellComponent.Cell;
            response.Position = pMovementComponent->Position;
            response.WorldSpaceId = cellComponent.WorldSpaceId;
        }
    }

    acMessage.pPlayer->Send(response);
}

void OverlayService::OnPlayerHealthUpdate(const PacketEvent<RequestPlayerHealthUpdate>& acMessage) const noexcept
{
    NotifyPlayerHealthUpdate notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.Percentage = acMessage.Packet.Percentage;

    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}
