#include "netplay.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/gpu_texture.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "digital_controller.h"
#include "host.h"
#include "host_settings.h"
#include "netplay_packets.h"
#include "pad.h"
#include "save_state_version.h"
#include "spu.h"
#include "system.h"
#include <bitset>
#include <deque>
#include <gsl/span>
#include <xxhash.h>
Log_SetChannel(Netplay);

#ifdef _WIN32
#include "common/windows_headers.h"
#pragma comment(lib, "ws2_32.lib")
#endif

// TODO: windows.h getting picked up somewhere here...
#include "enet/enet.h"
#include "ggponet.h"

namespace Netplay {

using SaveStateBuffer = std::unique_ptr<System::MemorySaveState>;

struct Input
{
  u32 button_data;
};

// TODO: Might be a bit generous... should we move this to config?
static constexpr float MAX_CONNECT_TIME = 15.0f;
static constexpr u32 MAX_CONNECT_RETRIES = 4;
static constexpr float MAX_CLOSE_TIME = 3.0f;

static bool NpAdvFrameCb(void* ctx, int flags);
static bool NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame);
static bool NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load);
static void NpFreeBuffCb(void* ctx, void* buffer, int frame);
static bool NpOnEventCb(void* ctx, GGPOEvent* ev);

static GGPOPlayerHandle PlayerIdToGGPOHandle(s32 player_id);
static Input ReadLocalInput();
static GGPOErrorCode AddLocalInput(Netplay::Input input);
static GGPOErrorCode SyncInput(Input inputs[2], int* disconnect_flags);
static void SetInputs(Input inputs[2]);

static void SetSettings();

static bool CreateSystem(std::string game_path, bool hosting);
static void CloseSessionWithError(const std::string_view& message);
static void RequestCloseSession(CloseSessionMessage::Reason reason);

// ENet
static bool InitializeEnet();
static void ShutdownEnetHost();
static std::string PeerAddressString(const ENetPeer* peer);
static void HandleEnetEvent(const ENetEvent* event);
static void PollEnet(Common::Timer::Value until_time);

// Player management
static bool IsValidPlayerId(s32 player_id);
static s32 GetFreePlayerId();
static ENetPeer* GetPeerForPlayer(s32 player_id);
static s32 GetPlayerIdForPeer(const ENetPeer* peer);
std::string_view GetNicknameForPlayer(s32 player_id);
static void NotifyPlayerJoined(s32 player_id);
static void DropPlayer(s32 player_id, DropPlayerReason reason);
static void ShowChatMessage(s32 player_id, const std::string_view& message);
static void RequestReset(ResetRequestMessage::Reason reason, s32 causing_player_id = 0);
static void SendConnectRequest();

// Controlpackets
static void HandleMessageFromNewPeer(ENetPeer* peer, const ENetPacket* pkt);
static void HandleControlMessage(s32 player_id, const ENetPacket* pkt);
static void HandleConnectResponseMessage(s32 player_id, const ENetPacket* pkt);
static void HandleResetMessage(s32 player_id, const ENetPacket* pkt);
static void HandleResetCompleteMessage(s32 player_id, const ENetPacket* pkt);
static void HandleResumeSessionMessage(s32 player_id, const ENetPacket* pkt);
static void HandleResetRequestMessage(s32 player_id, const ENetPacket* pkt);
static void HandlePlayerJoinedMessage(s32 player_id, const ENetPacket* pkt);
static void HandleDropPlayerMessage(s32 player_id, const ENetPacket* pkt);
static void HandleCloseSessionMessage(s32 player_id, const ENetPacket* pkt);
static void HandleChatMessage(s32 player_id, const ENetPacket* pkt);

// l = local, r = remote
static void CreateGGPOSession();
static void DestroyGGPOSession();
static bool Start(bool is_hosting, std::string nickname, const std::string& remote_addr, s32 port, s32 ldelay);
static void CloseSession();

// Host functions.
static void HandlePeerConnectionAsHost(ENetPeer* peer);
static void HandlePeerConnectionAsNonHost(ENetPeer* peer, s32 claimed_player_id);
static void HandlePeerDisconnectionAsHost(s32 player_id);
static void HandlePeerDisconnectionAsNonHost(s32 player_id);
static void Reset();
static void UpdateResetState();
static void UpdateConnectingState();

static void AdvanceFrame();
static void RunFrame();

static s32 CurrentFrame();

static void NetplayAdvanceFrame(Netplay::Input inputs[], int disconnect_flags);

/// Frame Pacing
static void InitializeFramePacing();
static void HandleTimeSyncEvent(float frame_delta, int update_interval);
static void Throttle();

// Desync Detection
static void GenerateChecksumForFrame(int* checksum, int frame, unsigned char* buffer, int buffer_size);

//////////////////////////////////////////////////////////////////////////
// Variables
//////////////////////////////////////////////////////////////////////////

static MemorySettingsInterface s_settings_overlay;
static SessionState s_state;

/// Enet
struct Peer
{
  ENetPeer* peer;
  std::string nickname;
  GGPOPlayerHandle ggpo_handle;
};
static ENetHost* s_enet_host = nullptr;
static std::array<Peer, MAX_PLAYERS> s_peers;
static s32 s_host_player_id = 0;
static s32 s_player_id = 0;
static s32 s_num_players = 0;
static u32 s_reset_cookie = 0;
static std::bitset<MAX_PLAYERS> s_reset_players;
static ENetAddress s_host_address;
static Common::Timer s_reset_start_time;
static Common::Timer s_last_host_connection_attempt;

/// GGPO
static std::string s_local_nickname;
static GGPOPlayerHandle s_local_handle = GGPO_INVALID_HANDLE;
static s32 s_local_delay = 0;
static GGPONetworkStats s_last_net_stats{};
static GGPOSession* s_ggpo = nullptr;

static std::deque<SaveStateBuffer> s_save_buffer_pool;

static std::array<std::array<float, 32>, NUM_CONTROLLER_AND_CARD_PORTS> s_net_input;

/// Frame timing. We manage our own frame pacing here, because we need to constantly adjust.
static float s_target_speed = 1.0f;
static Common::Timer::Value s_frame_period = 0;
static Common::Timer::Value s_next_frame_time = 0;
static s32 s_next_timesync_recovery_frame = -1;

//////////////////////////////////////////////////////////////////////////
// Packet helpers
//////////////////////////////////////////////////////////////////////////

static std::string DropPlayerReasonToString(DropPlayerReason reason)
{
  switch (reason)
  {
    case DropPlayerReason::ConnectTimeout:
      return Host::TranslateStdString("Netplay", "Connection timeout");
    case DropPlayerReason::DisconnectedFromHost:
      return Host::TranslateStdString("Netplay", "Disconnected from host");
    default:
      return "Unknown";
  }
}

template<typename T>
struct PacketWrapper
{
  ENetPacket* pkt;

  ALWAYS_INLINE const T* operator->() const { return reinterpret_cast<const T*>(pkt->data); }
  ALWAYS_INLINE T* operator->() { return reinterpret_cast<T*>(pkt->data); }
};
template<typename T>
static PacketWrapper<T> NewWrappedPacket(u32 size = sizeof(T), u32 flags = 0)
{
  return PacketWrapper<T>{enet_packet_create(nullptr, size, flags)};
}
template<typename T>
static PacketWrapper<T> NewControlPacket(u32 size = sizeof(T), u32 flags = ENET_PACKET_FLAG_RELIABLE)
{
  PacketWrapper<T> ret = NewWrappedPacket<T>(size, flags);
  ControlMessageHeader* hdr = reinterpret_cast<ControlMessageHeader*>(ret.pkt->data);
  hdr->type = T::MessageType();
  hdr->size = size;
  return ret;
}
template<typename T>
static bool SendControlPacket(ENetPeer* peer, const PacketWrapper<T>& pkt)
{
  const int rc = enet_peer_send(peer, ENET_CHANNEL_CONTROL, pkt.pkt);
  if (rc != 0)
  {
    Log_ErrorPrintf("enet_peer_send() failed: %d", rc);
    enet_packet_destroy(pkt.pkt);
    return false;
  }

  return true;
}
template<typename T>
static bool SendControlPacket(s32 player_id, const PacketWrapper<T>& pkt)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS && s_peers[player_id].peer);
  return SendControlPacket<T>(s_peers[player_id].peer, pkt);
}
template<typename T>
static void SendControlPacketToAll(const PacketWrapper<T>& pkt)
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!s_peers[i].peer)
      continue;

    // last one?
    bool last = true;
    for (s32 j = i + 1; j < MAX_PLAYERS; j++)
    {
      if (s_peers[j].peer)
      {
        last = false;
        break;
      }
    }

    ENetPacket* pkt_to_send = last ? pkt.pkt : enet_packet_create(pkt.pkt->data, pkt.pkt->dataLength, pkt.pkt->flags);
    const int rc = enet_peer_send(s_peers[i].peer, ENET_CHANNEL_CONTROL, pkt_to_send);
    if (rc != 0)
    {
      Log_ErrorPrintf("enet_peer_send() to player %d failed: %d", i, rc);
      enet_packet_destroy(pkt.pkt);
    }
  }
}
template<typename T>
static const T* CheckReceivedPacket(s32 player_id, const ENetPacket* pkt)
{
  if (pkt->dataLength < sizeof(T))
  {
    Log_ErrorPrintf("Received too-short control packet %u from player %d", static_cast<u32>(T::MessageType()),
                    player_id);
    return nullptr;
  }

  const ControlMessageHeader* hdr = reinterpret_cast<const ControlMessageHeader*>(pkt->data);
  if (hdr->size < sizeof(T))
  {
    Log_ErrorPrintf("Received too-short control packet %u from player %d [inner field]",
                    static_cast<u32>(T::MessageType()), player_id);
    return nullptr;
  }

  return reinterpret_cast<const T*>(pkt->data);
}
} // namespace Netplay

// Netplay Impl

bool Netplay::Start(bool is_hosting, std::string nickname, const std::string& remote_addr, s32 port, s32 ldelay)
{
  if (IsActive())
  {
    Log_ErrorPrintf("Netplay session already active");
    return false;
  }

  // Port should be valid regardless of hosting/joining.
  if (port < 0 || port >= 65535)
  {
    Log_ErrorPrintf("Invalid port %d", port);
    return false;
  }

  // Need a system if we're hosting.
  if (!System::IsValid())
  {
    if (is_hosting)
    {
      Log_ErrorPrintf("Can't host a netplay session without a valid VM");
      return false;
    }
    else if (!is_hosting && !CreateSystem(std::string(), false))
    {
      Log_ErrorPrintf("Failed to create VM for joining session");
      return false;
    }
  }

  s_state = SessionState::Initializing;

  SetSettings();

  if (!InitializeEnet())
  {
    Log_ErrorPrintf("Failed to initialize Enet.");
    return false;
  }

  // Create our "host" (which is basically just our port).
  ENetAddress server_address;
  server_address.host = ENET_HOST_ANY;
  server_address.port = is_hosting ? static_cast<u16>(port) : ENET_PORT_ANY;
  s_enet_host = enet_host_create(&server_address, MAX_PLAYERS - 1, NUM_ENET_CHANNELS, 0, 0);
  if (!s_enet_host)
  {
    Log_ErrorPrintf("Failed to create enet host.");
    return false;
  }

  s_host_player_id = 0;
  s_local_nickname = std::move(nickname);
  s_local_delay = ldelay;
  s_reset_cookie = 0;
  s_reset_players.reset();

  // If we're the host, we can just continue on our merry way, the others will join later.
  if (is_hosting)
  {
    // Starting session with a single player.
    s_player_id = 0;
    s_num_players = 1;
    s_reset_players = 1;
    CreateGGPOSession();
    s_state = SessionState::Running;
    Log_InfoPrintf("Netplay session started as host on port %d.", port);
    System::SetState(System::State::Paused);
    return true;
  }

  // for non-hosts, we don't know our player id yet until after we connect...
  s_player_id = -1;

  // Connect to host.
  s_host_address.port = static_cast<u16>(port);
  if (enet_address_set_host(&s_host_address, remote_addr.c_str()) != 0)
  {
    Log_ErrorPrintf("Failed to parse host: '%s'", remote_addr.c_str());
    return false;
  }

  s_peers[s_host_player_id].peer =
    enet_host_connect(s_enet_host, &s_host_address, NUM_ENET_CHANNELS, static_cast<u32>(s_player_id));
  if (!s_peers[s_host_player_id].peer)
  {
    Log_ErrorPrintf("Failed to start connection to host.");
    return false;
  }

  // Wait until we're connected to the main host. They'll send us back state to load and a full player list.
  s_state = SessionState::Connecting;
  s_reset_start_time.Reset();
  s_last_host_connection_attempt.Reset();
  System::SetState(System::State::Paused);
  return true;
}

void Netplay::SystemDestroyed()
{
  // something tried to shut us down..
  RequestCloseSession(CloseSessionMessage::Reason::HostShutdown);
}

void Netplay::CloseSession()
{
  Assert(IsActive() || s_state == SessionState::ClosingSession);

  const bool was_host = IsHost();

  DestroyGGPOSession();
  ShutdownEnetHost();

  // Restore original settings.
  Host::Internal::SetNetplaySettingsLayer(nullptr);
  System::ApplySettings(false);

  s_state = SessionState::Inactive;

  // Shut down the VM too, if we're not the host.
  if (!was_host)
    System::ShutdownSystem(false);
}

bool Netplay::IsActive()
{
  return (s_state >= SessionState::Initializing && s_state <= SessionState::Running);
}

bool Netplay::CreateSystem(std::string game_path, bool is_hosting)
{
  // close system if its already running
  if (System::IsValid())
    System::ShutdownSystem(false);

  // fast boot the selected game and wait for the other player
  auto param = SystemBootParameters(std::move(game_path));
  param.override_fast_boot = true;
  if (!System::BootSystem(param))
    return false;

  if (is_hosting)
  {
    // Fast Forward to Game Start if needed.
    SPU::SetAudioOutputMuted(true);
    while (System::GetInternalFrameNumber() < 2)
      System::RunFrame();
    SPU::SetAudioOutputMuted(false);
  }

  return true;
}

void Netplay::CloseSessionWithError(const std::string_view& message)
{
  Host::ReportErrorAsync(Host::TranslateString("Netplay", "Netplay Error"), message);
  s_state = SessionState::ClosingSession;
}

void Netplay::RequestCloseSession(CloseSessionMessage::Reason reason)
{
  if (IsHost())
  {
    // Notify everyone
    auto pkt = NewControlPacket<CloseSessionMessage>();
    pkt->reason = reason;
    SendControlPacketToAll(pkt);
  }

  // Close all connections
  DestroyGGPOSession();
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_peers[i].peer)
    {
      if (IsHost())
        enet_peer_disconnect_later(s_peers[i].peer, 0);
      else
        enet_peer_disconnect(s_peers[i].peer, 0);
    }
  }

  // but wait for them to actually drop
  s_state = SessionState::ClosingSession;
  s_reset_start_time.Reset();

  // if we have a system, we can display the visual, otherwise just get out of here
  // that might happen if they click shutdown, then shutdown again and don't wait
  while (System::IsValid() && s_reset_start_time.GetTimeSeconds() < MAX_CLOSE_TIME)
  {
    // Just check that all players have disconnected.
    // We don't want to handle any requests here.
    ENetEvent event;
    if (enet_host_service(s_enet_host, &event, 1) >= 0)
    {
      switch (event.type)
      {
        case ENET_EVENT_TYPE_DISCONNECT:
        {
          const s32 player_id = GetPlayerIdForPeer(event.peer);
          if (player_id >= 0)
            s_peers[player_id].peer = nullptr;
        }
        break;

        case ENET_EVENT_TYPE_RECEIVE:
        {
          // Discard all packets.
          enet_packet_destroy(event.packet);
        }
        break;

        default:
          break;
      }
    }

    if (std::none_of(s_peers.begin(), s_peers.end(), [](const Peer& p) { return p.peer != nullptr; }))
      return;

    Host::DisplayLoadingScreen("Closing session");
    Host::PumpMessagesOnCPUThread();
  }
}

bool Netplay::InitializeEnet()
{
  static bool enet_initialized = false;
  int rc;
  if (!enet_initialized && (rc = enet_initialize()) != 0)
  {
    Log_ErrorPrintf("enet_initialize() returned %d", rc);
    return false;
  }

  std::atexit(enet_deinitialize);
  enet_initialized = true;
  return true;
}

void Netplay::ShutdownEnetHost()
{
  if (!s_enet_host)
    return;

  Log_DevPrint("Shutting down Enet host");
  for (u32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_peers[i].peer)
      enet_peer_reset(s_peers[i].peer);

    s_peers[i] = {};
  }

  enet_host_destroy(s_enet_host);
  s_enet_host = nullptr;
}

std::string Netplay::PeerAddressString(const ENetPeer* peer)
{
  char buf[128];
  if (enet_address_get_host_ip(&peer->address, buf, std::size(buf)))
    buf[0] = 0;

  return fmt::format("{}:{}", buf, peer->address.port);
}

void Netplay::HandleEnetEvent(const ENetEvent* event)
{
  switch (event->type)
  {
    case ENET_EVENT_TYPE_CONNECT:
    {
      if (IsHost())
        HandlePeerConnectionAsHost(event->peer);
      else
        HandlePeerConnectionAsNonHost(event->peer, static_cast<s32>(event->data));

      return;
    }
    break;

    case ENET_EVENT_TYPE_DISCONNECT:
    {
      const s32 player_id = GetPlayerIdForPeer(event->peer);
      if (s_state == SessionState::Connecting)
      {
        Assert(player_id == s_host_player_id);
        Panic("Failed to connect to host");
        return;
      }
      else if (s_state == SessionState::Resetting)
      {
        // let the timeout deal with it
        Log_DevPrintf("Ignoring disconnection from %d while synchronizing", player_id);
        return;
      }

      Log_WarningPrintf("ENet player %d disconnected", player_id);
      if (IsValidPlayerId(player_id))
      {
        if (IsHost())
          HandlePeerDisconnectionAsHost(player_id);
        else
          HandlePeerDisconnectionAsNonHost(player_id);
      }
    }
    break;

    case ENET_EVENT_TYPE_RECEIVE:
    {
      const s32 player_id = GetPlayerIdForPeer(event->peer);
      if (player_id < 0)
      {
        // If it's a new connection, we need to handle connection request messages.
        if (event->channelID == ENET_CHANNEL_CONTROL)
          HandleMessageFromNewPeer(event->peer, event->packet);
        enet_packet_destroy(event->packet);
        return;
      }

      if (event->channelID == ENET_CHANNEL_CONTROL)
      {
        HandleControlMessage(player_id, event->packet);
      }
      else if (event->channelID == ENET_CHANNEL_GGPO)
      {
        Log_TracePrintf("Received %zu ggpo bytes from player %d", event->packet->dataLength, player_id);
        const int rc = ggpo_handle_packet(s_ggpo, event->peer, event->packet);
        if (rc != GGPO_OK)
          Log_ErrorPrintf("Failed to process GGPO packet!");
      }
      else
      {
        Log_ErrorPrintf("Unexpected packet channel %u", event->channelID);
      }

      enet_packet_destroy(event->packet);
    }
    break;

    default:
    {
      Log_WarningPrintf("Unhandled enet event %d", event->type);
    }
    break;
  }
}

void Netplay::PollEnet(Common::Timer::Value until_time)
{
  ENetEvent event;

  u64 current_time = Common::Timer::GetCurrentValue();

  while (IsActive())
  {
    const u32 enet_timeout = (current_time >= until_time) ?
                               0 :
                               static_cast<u32>(Common::Timer::ConvertValueToMilliseconds(until_time - current_time));

    // make sure s_enet_host exists
    Assert(s_enet_host);

    const int res = enet_host_service(s_enet_host, &event, enet_timeout);
    if (res > 0)
    {
      HandleEnetEvent(&event);

      // receiving can trigger sending
      if (s_ggpo)
        ggpo_network_idle(s_ggpo);

      // make sure we get all events
      current_time = Common::Timer::GetCurrentValue();
      continue;
    }
    // exit once we're nonblocking
    current_time = Common::Timer::GetCurrentValue();
    if (enet_timeout == 0 || current_time >= until_time)
      break;
  }
}

bool Netplay::IsHost()
{
  return s_player_id == s_host_player_id;
}

GGPOPlayerHandle Netplay::PlayerIdToGGPOHandle(s32 player_id)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS);
  return s_peers[player_id].ggpo_handle;
}

ENetPeer* Netplay::GetPeerForPlayer(s32 player_id)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS);
  return s_peers[player_id].peer;
}

s32 Netplay::GetPlayerIdForPeer(const ENetPeer* peer)
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_peers[i].peer == peer)
      return i;
  }

  return -1;
}

bool Netplay::IsValidPlayerId(s32 player_id)
{
  return s_player_id == player_id || (player_id >= 0 && player_id < MAX_PLAYERS && s_peers[player_id].peer);
}

s32 Netplay::GetFreePlayerId()
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (i != s_player_id && !s_peers[i].peer)
      return i;
  }

  return -1;
}

std::string_view Netplay::GetNicknameForPlayer(s32 player_id)
{
  return IsValidPlayerId(player_id) ?
           std::string_view(player_id == s_player_id ? s_local_nickname : s_peers[player_id].nickname) :
           std::string_view();
}

void Netplay::CreateGGPOSession()
{
  /*
  TODO: since saving every frame during rollback loses us time to do actual gamestate iterations it might be better to
  hijack the update / save / load cycle to only save every confirmed frame only saving when actually needed.
  */
  GGPOSessionCallbacks cb = {};
  cb.advance_frame = NpAdvFrameCb;
  cb.save_game_state = NpSaveFrameCb;
  cb.load_game_state = NpLoadFrameCb;
  cb.free_buffer = NpFreeBuffCb;
  cb.on_event = NpOnEventCb;

  ggpo_start_session(&s_ggpo, &cb, s_num_players, sizeof(Netplay::Input), MAX_ROLLBACK_FRAMES);

  int player_num = 1;
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    // slot filled?
    if (!s_peers[i].peer && i != s_player_id)
      continue;

    GGPOPlayer player = {sizeof(GGPOPlayer)};
    GGPOErrorCode result;
    player.player_num = player_num++;
    if (i == s_player_id)
    {
      player.type = GGPO_PLAYERTYPE_LOCAL;
      result = ggpo_add_player(s_ggpo, &player, &s_peers[i].ggpo_handle);
      if (GGPO_SUCCEEDED(result))
        s_local_handle = s_peers[i].ggpo_handle;
    }
    else
    {
      player.type = GGPO_PLAYERTYPE_REMOTE;
      player.u.remote.peer = s_peers[i].peer;
      result = ggpo_add_player(s_ggpo, &player, &s_peers[i].ggpo_handle);
    }

    // It's a new session, this should always succeed...
    Assert(GGPO_SUCCEEDED(result));
  }

  ggpo_set_frame_delay(s_ggpo, s_local_handle, s_local_delay);
  InitializeFramePacing();
}

void Netplay::DestroyGGPOSession()
{
  if (!s_ggpo)
    return;

  Log_DevPrintf("Destroying GGPO session...");
  ggpo_close_session(s_ggpo);
  s_ggpo = nullptr;
  s_save_buffer_pool.clear();
  s_local_handle = GGPO_INVALID_HANDLE;

  for (Peer& p : s_peers)
    p.ggpo_handle = GGPO_INVALID_HANDLE;
}

void Netplay::HandleControlMessage(s32 player_id, const ENetPacket* pkt)
{
  if (pkt->dataLength < sizeof(ControlMessageHeader))
  {
    Log_ErrorPrintf("Invalid control packet from player %d of size %zu", player_id, pkt->dataLength);
    return;
  }

  const ControlMessageHeader* hdr = reinterpret_cast<const ControlMessageHeader*>(pkt->data);
  switch (hdr->type)
  {
    case ControlMessage::ConnectResponse:
      HandleConnectResponseMessage(player_id, pkt);
      break;

    case ControlMessage::Reset:
      HandleResetMessage(player_id, pkt);
      break;

    case ControlMessage::ResetComplete:
      HandleResetCompleteMessage(player_id, pkt);
      break;

    case ControlMessage::ResumeSession:
      HandleResumeSessionMessage(player_id, pkt);
      break;

    case ControlMessage::PlayerJoined:
      HandlePlayerJoinedMessage(player_id, pkt);
      break;

    case ControlMessage::DropPlayer:
      HandleDropPlayerMessage(player_id, pkt);
      break;

    case ControlMessage::ResetRequest:
      HandleResetRequestMessage(player_id, pkt);
      break;

    case ControlMessage::CloseSession:
      HandleCloseSessionMessage(player_id, pkt);
      break;

    case ControlMessage::ChatMessage:
      HandleChatMessage(player_id, pkt);
      break;

    default:
      Log_ErrorPrintf("Unhandled control packet %u from player %d of size %zu", hdr->type, player_id, pkt->dataLength);
      break;
  }
}

void Netplay::HandlePeerConnectionAsHost(ENetPeer* peer)
{
  // don't do anything until they send a connect request
  // TODO: we might want to put an idle timeout here...
  Log_InfoPrintf(fmt::format("New peer connection from {}", PeerAddressString(peer)).c_str());
}

void Netplay::HandleMessageFromNewPeer(ENetPeer* peer, const ENetPacket* pkt)
{
  const ConnectRequestMessage* msg = CheckReceivedPacket<ConnectRequestMessage>(-1, pkt);
  if (!msg || msg->header.type != ControlMessage::ConnectRequest)
  {
    Log_WarningPrintf("Received unknown packet from unknown player");
    enet_peer_reset(peer);
    return;
  }

  Log_VerbosePrint(fmt::format("New host peer connection from {} claiming player ID {}", PeerAddressString(peer),
                               msg->requested_player_id)
                     .c_str());

  PacketWrapper<ConnectResponseMessage> response = NewControlPacket<ConnectResponseMessage>();
  response->player_id = -1;

  // TODO: Spectators shouldn't get assigned a real player ID, they should go into a separate peer list.
  if (msg->mode != ConnectRequestMessage::Mode::Player)
  {
    response->result = ConnectResponseMessage::Result::SessionClosed;
    SendControlPacket(peer, response);
    return;
  }

  // Player is attempting to reconnect.
  // Hopefully both sides have disconnected completely before they reconnect.
  // If not, too bad.
  if (msg->requested_player_id >= 0 && IsValidPlayerId(msg->requested_player_id))
  {
    Log_ErrorPrintf("Player ID %d is already in use, rejecting connection.", msg->requested_player_id);
    response->result = ConnectResponseMessage::Result::PlayerIDInUse;
    SendControlPacket(peer, response);
    return;
  }

  // Any free slots?
  const s32 new_player_id = (msg->requested_player_id >= 0) ? msg->requested_player_id : GetFreePlayerId();
  if (new_player_id < 0)
  {
    Log_ErrorPrintf("Server full, rejecting connection.");
    response->result = ConnectResponseMessage::Result::ServerFull;
    SendControlPacket(peer, response);
    return;
  }

  Log_VerbosePrint(
    fmt::format("New connection from {} assigned to player ID {}", PeerAddressString(peer), new_player_id).c_str());
  response->result = ConnectResponseMessage::Result::Success;
  response->player_id = new_player_id;
  SendControlPacket(peer, response);

  // Set up their player slot.
  Assert(s_num_players < MAX_PLAYERS);
  s_peers[new_player_id].peer = peer;
  s_peers[new_player_id].nickname = msg->GetNickname();
  s_num_players++;

  // Force everyone to resynchronize with the new player.
  Reset();

  // Notify *after* reset so they have their nickname.
  NotifyPlayerJoined(new_player_id);
}

void Netplay::HandlePeerConnectionAsNonHost(ENetPeer* peer, s32 claimed_player_id)
{
  if (s_state == SessionState::Connecting)
  {
    if (peer == s_peers[s_host_player_id].peer)
    {
      SendConnectRequest();
      return;
    }
    else
    {
      Log_ErrorPrintf(
        fmt::format("Unexpected connection from {} claiming player ID {}", PeerAddressString(peer), claimed_player_id)
          .c_str());
      enet_peer_disconnect_now(peer, 0);
      return;
    }
  }

  Log_VerbosePrint(
    fmt::format("New peer connection from {} claiming player ID {}", PeerAddressString(peer), claimed_player_id)
      .c_str());

  // shouldn't ever get a non-host connection without a valid ID
  if (claimed_player_id < 0 || claimed_player_id >= MAX_PLAYERS || claimed_player_id == s_player_id)
  {
    Log_ErrorPrintf("Invalid claimed_player_id %d", claimed_player_id);
    enet_peer_disconnect_now(peer, 0);
    return;
  }

  // the peer should match up, unless we're the one receiving the connection
  Assert(s_peers[claimed_player_id].peer == peer || claimed_player_id > s_player_id);
  if (s_peers[claimed_player_id].peer == peer)
  {
    // WaitForPeerConnections handles this case.
    // If this was the host, we still need to wait for the synchronization.
    Log_DevPrintf("Connection complete with player %d%s", claimed_player_id,
                  (claimed_player_id == s_host_player_id) ? "[HOST]" : "");
    return;
  }

  Log_DevPrintf("Connection received from peer %d", claimed_player_id);
  s_peers[claimed_player_id].peer = peer;
}

void Netplay::SendConnectRequest()
{
  DebugAssert(!IsHost());

  Log_DevPrintf("Sending connect request to host with player id %d", s_player_id);

  auto pkt = NewControlPacket<ConnectRequestMessage>();
  pkt->mode = ConnectRequestMessage::Mode::Player;
  pkt->requested_player_id = s_player_id;
  std::memset(pkt->nickname, 0, sizeof(pkt->nickname));
  std::memset(pkt->session_password, 0, sizeof(pkt->session_password));
  StringUtil::Strlcpy(pkt->nickname, s_local_nickname, std::size(pkt->nickname));
  SendControlPacket(s_peers[s_host_player_id].peer, pkt);
}

void Netplay::UpdateConnectingState()
{
  if (s_reset_start_time.GetTimeSeconds() >= MAX_CONNECT_TIME)
  {
    CloseSessionWithError(Host::TranslateStdString("Netplay", "Timed out connecting to server."));
    return;
  }

  // MAX_CONNECT_RETRIES peer to host connection attempts
  // dividing by MAX_CONNECT_RETRIES + 1 because the last attempt will never happen.
  if (s_last_host_connection_attempt.GetTimeSeconds() > MAX_CONNECT_TIME / (MAX_CONNECT_RETRIES + 1) &&
      s_peers[s_host_player_id].peer->state != ENetPeerState::ENET_PEER_STATE_CONNECTED)
  {
    // we want to do this because the peer might have initiated a connection
    // too early while the host was still setting up. this gives the connection MAX_CONNECT_RETRIES tries within
    // MAX_CONNECT_TIME to establish the connection
    enet_peer_reset(s_peers[s_host_player_id].peer);
    enet_host_connect(s_enet_host, &s_host_address, NUM_ENET_CHANNELS, static_cast<u32>(s_player_id));
    s_last_host_connection_attempt.Reset();
  }

  // still waiting for connection to host..
  PollEnet(Common::Timer::GetCurrentValue() + Common::Timer::ConvertMillisecondsToValue(16));
  Host::DisplayLoadingScreen("Connecting to host...");
  Host::PumpMessagesOnCPUThread();
}

void Netplay::HandleConnectResponseMessage(s32 player_id, const ENetPacket* pkt)
{
  if (s_state != SessionState::Connecting)
  {
    Log_ErrorPrintf("Received unexpected connect response from player %d", player_id);
    return;
  }

  const ConnectResponseMessage* msg = CheckReceivedPacket<ConnectResponseMessage>(player_id, pkt);
  if (msg->result != ConnectResponseMessage::Result::Success)
  {
    CloseSessionWithError(
      fmt::format("Connection rejected by server with error code {}", static_cast<u32>(msg->result)));
    return;
  }

  // Still need to wait for synchronization
  Log_InfoPrintf("Connected to host, we were assigned player ID %d", msg->player_id);
  s_player_id = msg->player_id;
  s_state = SessionState::Resetting;
  s_reset_players.reset();
  s_reset_start_time.Reset();
}

void Netplay::HandlePeerDisconnectionAsHost(s32 player_id)
{
  Log_InfoPrintf("Player %d disconnected from host, reclaiming their slot", player_id);
  DropPlayer(player_id, DropPlayerReason::DisconnectedFromHost);
}

void Netplay::HandlePeerDisconnectionAsNonHost(s32 player_id)
{
  Log_InfoPrintf("Lost connection with player %d", player_id);
  if (player_id == s_host_player_id)
  {
    // TODO: Automatically try to reconnect to the host with our existing player ID.
    CloseSessionWithError(Host::TranslateStdString("Netplay", "Lost connection to host"));
    return;
  }

  // tell the host we dropped a connection, let them deal with it..
  RequestReset(ResetRequestMessage::Reason::ConnectionLost, player_id);
}

void Netplay::Reset()
{
  Assert(IsHost());

  Log_VerbosePrintf("Resetting...");

  // Use the current system state, whatever that is.
  // TODO: This save state has the bloody path to the disc in it. We need a new save state format.
  // We also want to use maximum compression.
  GrowableMemoryByteStream state(nullptr, System::MAX_SAVE_STATE_SIZE);
  if (!System::SaveStateToStream(&state, 0, SAVE_STATE_HEADER::COMPRESSION_TYPE_ZSTD))
    Panic("Failed to save state...");

  const u32 state_data_size = static_cast<u32>(state.GetPosition());
  ResetMessage header = {};
  header.header.type = ControlMessage::Reset;
  header.header.size = sizeof(ResetMessage) + state_data_size;
  header.state_data_size = state_data_size;
  header.cookie = ++s_reset_cookie;

  // Fill in player info.
  header.num_players = s_num_players;
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!IsValidPlayerId(i))
    {
      header.players[i].controller_port = -1;
      continue;
    }

    // TODO: This is wrong, so wrong....
    header.players[i].controller_port = static_cast<s16>(i);
    StringUtil::Strlcpy(header.players[i].nickname, s_peers[i].nickname, std::size(header.players[i].nickname));

    if (i == s_player_id)
    {
      // TODO: not valid... listening on any address.
      header.players[i].host = s_enet_host->address.host;
      header.players[i].port = s_enet_host->address.port;
    }
    else
    {
      header.players[i].host = s_peers[i].peer->address.host;
      header.players[i].port = s_peers[i].peer->address.port;
    }
  }

  // Distribute sync request to all peers, then wait for them to reload back.
  // Any GGPO packets will get dropped, since the session's gone temporarily.
  DestroyGGPOSession();

  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!s_peers[i].peer)
      continue;

    ENetPacket* pkt = enet_packet_create(nullptr, sizeof(header) + state_data_size, ENET_PACKET_FLAG_RELIABLE);
    std::memcpy(pkt->data, &header, sizeof(header));
    std::memcpy(pkt->data + sizeof(header), state.GetMemoryPointer(), state_data_size);

    // This should never fail, we get errors back later..
    const int rc = enet_peer_send(s_peers[i].peer, ENET_CHANNEL_CONTROL, pkt);
    if (rc != 0)
      Log_ErrorPrintf("enet_peer_send() for synchronization request failed: %d", rc);
  }

  // Do a full state reload on the host as well, that way everything (including the GPU)
  // has a clean slate, reducing the chance of desyncs. Looking at you, mister rec, somehow
  // having a different number of cycles.
  // CPU::CodeCache::Flush();
  state.SeekAbsolute(0);
  if (!System::LoadStateFromStream(&state, true))
    Panic("Failed to reload host state");

  s_state = SessionState::Resetting;
  s_reset_players.reset();
  s_reset_players.set(s_player_id);
  s_reset_start_time.Reset();
}

void Netplay::HandleResetMessage(s32 player_id, const ENetPacket* pkt)
{
  if (player_id != s_host_player_id)
  {
    // This shouldn't ever happen, unless someone's being cheeky.
    Log_ErrorPrintf("Dropping reset from non-host player %d", player_id);
    return;
  }

  const ResetMessage* msg = reinterpret_cast<const ResetMessage*>(pkt->data);
  if (pkt->dataLength < sizeof(ResetMessage) || pkt->dataLength < (sizeof(ResetMessage) + msg->state_data_size))
  {
    CloseSessionWithError(fmt::format("Invalid synchronization request: expected {} bytes, got {} bytes",
                                      sizeof(ResetMessage) + msg->state_data_size, pkt->dataLength));
    return;
  }

  DestroyGGPOSession();

  // Make sure we're connected to all players.
  Assert(msg->num_players > 1);
  Log_DevPrintf("Checking connections");
  s_num_players = msg->num_players;
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    Peer& p = s_peers[i];
    if (msg->players[i].controller_port < 0)
    {
      // If we had a client here, it must've dropped or something..
      if (p.peer)
      {
        Log_WarningPrintf("Dropping connection to player %d", i);
        enet_peer_disconnect_now(p.peer, 0);
        p.peer = nullptr;
      }

      continue;
    }

    // Can't connect to ourselves!
    if (i == s_player_id)
      continue;

    // Update nickname.
    p.nickname = msg->players[i].GetNickname();

    // Or the host, which may not contain a correct address, since it's from itself.
    if (i == s_host_player_id)
      continue;

    // The host should fall into the category where we can reuse.
    if (p.peer && p.peer->address.host == msg->players[i].host && p.peer->address.port == msg->players[i].port)
    {
      Log_DevPrintf("Preserving connection to player %d", i);
      continue;
    }

    Assert(i != s_host_player_id);
    if (p.peer)
    {
      enet_peer_reset(p.peer);
      p.peer = nullptr;
    }

    // If this player has a higher ID than us, then they're responsible for connecting to us, not the other way around.
    // i.e. player 2 is responsible for connecting to players 0 and 1, player 1 is responsible for connecting to player
    // 0.
    if (i > s_player_id)
    {
      Log_DevPrintf("Not connecting to player %d, as they have a higher ID than us (%d)", i);
      continue;
    }

    const ENetAddress address = {msg->players[i].host, msg->players[i].port};
    p.peer = enet_host_connect(s_enet_host, &address, NUM_ENET_CHANNELS, static_cast<u32>(s_player_id));
    if (!p.peer)
      Panic("Failed to connect to peer on resynchronize");
  }

  // Load state from packet.
  Log_VerbosePrintf("Loading state from host");
  ReadOnlyMemoryByteStream stream(pkt->data + sizeof(ResetMessage), msg->state_data_size);
  if (!System::LoadStateFromStream(&stream, true))
    Panic("Failed to load state from host");

  s_state = SessionState::Resetting;
  s_reset_cookie = msg->cookie;
  s_reset_players.reset();
  s_reset_players.set(s_player_id);
  s_reset_start_time.Reset();
}

void Netplay::HandleResetCompleteMessage(s32 player_id, const ENetPacket* pkt)
{
  const ResetCompleteMessage* msg = CheckReceivedPacket<ResetCompleteMessage>(player_id, pkt);
  if (!msg)
    return;

  if (s_state != SessionState::Resetting || player_id == s_host_player_id)
  {
    Log_ErrorPrintf("Received unexpected reset complete from player %d", player_id);
    return;
  }
  else if (s_reset_players.test(player_id))
  {
    Log_ErrorPrintf("Received double reset from player %d", player_id);
    return;
  }
  else if (s_reset_cookie != msg->cookie)
  {
    Log_ErrorPrintf("Incorrect reset cookie sent from player %d", player_id);
    return;
  }

  Log_DevPrintf("Player %d is now reset and ready", player_id);
  s_reset_players.set(player_id);
}

void Netplay::HandleResumeSessionMessage(s32 player_id, const ENetPacket* pkt)
{
  const ResumeSessionMessage* msg = CheckReceivedPacket<ResumeSessionMessage>(player_id, pkt);
  if (!msg)
    return;

  if (s_state != SessionState::Resetting || player_id != s_host_player_id)
  {
    Log_ErrorPrintf("Received unexpected resume session from player %d", player_id);
    return;
  }

  Log_DevPrintf("Resuming session");
  CreateGGPOSession();
  s_state = SessionState::Running;
}

void Netplay::UpdateResetState()
{
  if (IsHost())
  {
    if (static_cast<s32>(s_reset_players.count()) == s_num_players)
    {
      Log_VerbosePrintf("All players synchronized, resuming!");
      SendControlPacketToAll(NewControlPacket<ResumeSessionMessage>());
      CreateGGPOSession();
      s_state = SessionState::Running;
      return;
    }

    // connect timeout exceeded?
    if (s_reset_start_time.GetTimeSeconds() >= MAX_CONNECT_TIME)
    {
      // TODO: this should be tweaked, maybe only drop one at a time?
      Log_InfoPrintf("Reset timeout, dropping any players who aren't connected");
      for (s32 i = 0; i < MAX_PLAYERS; i++)
      {
        if (!IsValidPlayerId(i) || s_reset_players.test(i))
          continue;

        // we'll check if we're done again next loop
        Log_DevPrintf("Dropping player %d because they didn't connect in time", i);
        DropPlayer(i, DropPlayerReason::ConnectTimeout);
      }
    }
  }
  else
  {
    if (static_cast<s32>(s_reset_players.count()) != s_num_players)
    {
      for (s32 i = 0; i < MAX_PLAYERS; i++)
      {
        if (!IsValidPlayerId(i) || s_reset_players.test(i))
          continue;
        // be sure to first check whether the peer is still valid.
        if (s_peers[i].peer && s_peers[i].peer->state == ENET_PEER_STATE_CONNECTED)
          s_reset_players.set(i);
      }

      if (static_cast<s32>(s_reset_players.count()) == s_num_players)
      {
        // now connected to all!
        Log_InfoPrintf("Connected to %d players, waiting for host...", s_num_players);
        auto pkt = NewControlPacket<ResetCompleteMessage>();
        pkt->cookie = s_reset_cookie;
        SendControlPacket(s_host_player_id, pkt);
      }

      // cancel ourselves if we didn't get another synchronization request from the host
      if (s_reset_start_time.GetTimeSeconds() >= (MAX_CONNECT_TIME * 2.0f))
      {
        CloseSessionWithError(Host::TranslateStdString("Netplay", "Failed to connect within timeout"));
        return;
      }
    }
  }

  PollEnet(Common::Timer::GetCurrentValue() + Common::Timer::ConvertMillisecondsToValue(16));
  Host::DisplayLoadingScreen("Netplay synchronizing", 0, static_cast<int>(s_reset_players.count()), s_num_players);
  Host::PumpMessagesOnCPUThread();
}

void Netplay::RequestReset(ResetRequestMessage::Reason reason, s32 causing_player_id /* = 0 */)
{
  Assert(!IsHost());

  auto pkt = NewControlPacket<ResetRequestMessage>();
  pkt->reason = reason;
  pkt->causing_player_id = causing_player_id;

  Log_DevPrintf("Requesting reset from host due to %s", pkt->ReasonToString().c_str());
  SendControlPacket(s_host_player_id, pkt);
}

void Netplay::HandleResetRequestMessage(s32 player_id, const ENetPacket* pkt)
{
  const ResetRequestMessage* msg = CheckReceivedPacket<ResetRequestMessage>(player_id, pkt);
  if (!msg)
    return;

  Log_InfoPrintf("Received reset request from player %d due to %s", player_id, msg->ReasonToString().c_str());
  Reset();
}

void Netplay::NotifyPlayerJoined(s32 player_id)
{
  if (IsHost())
  {
    auto pkt = NewControlPacket<PlayerJoinedMessage>();
    pkt->player_id = player_id;
    SendControlPacketToAll(pkt);
  }

  Host::OnNetplayMessage(
    fmt::format(Host::TranslateString("Netplay", "{} is joining the session as player {}.").GetCharArray(),
                GetNicknameForPlayer(player_id), player_id));
}

void Netplay::HandlePlayerJoinedMessage(s32 player_id, const ENetPacket* pkt)
{
  const PlayerJoinedMessage* msg = CheckReceivedPacket<PlayerJoinedMessage>(player_id, pkt);
  if (!msg || player_id != s_host_player_id)
    return;

  NotifyPlayerJoined(msg->player_id);
}

void Netplay::DropPlayer(s32 player_id, DropPlayerReason reason)
{
  Assert(IsValidPlayerId(player_id) && s_host_player_id != player_id && s_player_id != player_id);
  DebugAssert(s_peers[player_id].peer);

  Log_InfoPrintf("Dropping player %d", player_id);

  Host::OnNetplayMessage(fmt::format(Host::TranslateString("Netplay", "{} left the session: {}").GetCharArray(),
                                     GetNicknameForPlayer(player_id), DropPlayerReasonToString(reason)));

  enet_peer_disconnect_now(s_peers[player_id].peer, 0);
  s_peers[player_id] = {};
  s_num_players--;

  if (!IsHost())
  {
    // if we're not the host, the host should send a resynchronize request shortly
    // but enter the state early, that way we don't keep sending ggpo stuff...
    DestroyGGPOSession();
    s_state = SessionState::Resetting;
  }
  else
  {
    // tell who's left to also drop their side
    auto pkt = NewControlPacket<DropPlayerMessage>();
    pkt->reason = reason;
    pkt->player_id = player_id;
    SendControlPacketToAll(pkt);

    // resync with everyone who's left
    Reset();
  }
}

void Netplay::HandleDropPlayerMessage(s32 player_id, const ENetPacket* pkt)
{
  const DropPlayerMessage* msg = CheckReceivedPacket<DropPlayerMessage>(player_id, pkt);
  if (!msg)
    return;

  if (player_id != s_host_player_id)
  {
    Log_ErrorPrintf("Received unexpected drop player from player %d", player_id);
    return;
  }

  DropPlayer(player_id, msg->reason);
}

void Netplay::HandleCloseSessionMessage(s32 player_id, const ENetPacket* pkt)
{
  const CloseSessionMessage* msg = CheckReceivedPacket<CloseSessionMessage>(player_id, pkt);
  if (!msg)
    return;

  Host::ReportErrorAsync(Host::TranslateString("Netplay", "Netplay Session Ended"), msg->ReasonToString());
  RequestCloseSession(msg->reason);
}

void Netplay::ShowChatMessage(s32 player_id, const std::string_view& message)
{
  if (!message.empty())
    Host::OnNetplayMessage(fmt::format("{}: {}", GetNicknameForPlayer(player_id), message));
}

void Netplay::HandleChatMessage(s32 player_id, const ENetPacket* pkt)
{
  const ChatMessage* msg = CheckReceivedPacket<ChatMessage>(player_id, pkt);
  if (!msg)
    return;

  ShowChatMessage(player_id, msg->GetMessage());
}

//////////////////////////////////////////////////////////////////////////
// Settings Overlay
//////////////////////////////////////////////////////////////////////////

void Netplay::SetSettings()
{
  MemorySettingsInterface& si = s_settings_overlay;

  si.Clear();
  for (u32 i = 0; i < MAX_PLAYERS; i++)
  {
    // Only digital pads supported for now.
    si.SetStringValue(Controller::GetSettingsSection(i).c_str(), "Type",
                      Settings::GetControllerTypeName(ControllerType::DigitalController));
  }

  // si.SetStringValue("CPU", "ExecutionMode", "Interpreter");

  // No runahead or rewind, that'd be a disaster.
  si.SetIntValue("Main", "RunaheadFrameCount", 0);
  si.SetBoolValue("Main", "RewindEnable", false);

  // no block linking, it degrades savestate loading performance
  si.SetBoolValue("CPU", "RecompilerBlockLinking", false);
  // not sure its needed but enabled for now... TODO
  si.SetBoolValue("GPU", "UseSoftwareRendererForReadbacks", true);

  // TODO: PGXP should be the same as the host, as should overclock etc.

  Host::Internal::SetNetplaySettingsLayer(&si);
  System::ApplySettings(false);
}

//////////////////////////////////////////////////////////////////////////
// Frame Pacing
//////////////////////////////////////////////////////////////////////////

void Netplay::InitializeFramePacing()
{
  // Start at 100% speed, adjust as soon as we get a timesync event.
  s_target_speed = 1.0f;
  UpdateThrottlePeriod();

  s_next_frame_time = Common::Timer::GetCurrentValue() + s_frame_period;
}

void Netplay::UpdateThrottlePeriod()
{
  s_frame_period =
    Common::Timer::ConvertSecondsToValue(1.0 / (static_cast<double>(System::GetThrottleFrequency()) * s_target_speed));
}

void Netplay::HandleTimeSyncEvent(float frame_delta, int update_interval)
{
  // only activate timesync if its worth correcting.
  if (std::abs(frame_delta) < 1.0f)
    return;
  // Distribute the frame difference over the next N * 0.75 frames.
  // only part of the interval time is used since we want to come back to normal speed.
  // otherwise we will keep spiraling into unplayable gameplay.
  float total_time = (frame_delta * s_frame_period) / 4;
  float mun_timesync_frames = update_interval * 0.75f;
  float added_time_per_frame = -(total_time / mun_timesync_frames);
  float iterations_per_frame = 1.0f / s_frame_period;

  s_target_speed = (s_frame_period + added_time_per_frame) * iterations_per_frame;
  s_next_timesync_recovery_frame = CurrentFrame() + static_cast<s32>(std::ceil(mun_timesync_frames));

  UpdateThrottlePeriod();

  Log_VerbosePrintf("TimeSync: %f frames %s, target speed %.4f%%", std::abs(frame_delta),
                    (frame_delta >= 0.0f ? "ahead" : "behind"), s_target_speed * 100.0f);
}

void Netplay::Throttle()
{
  // if the s_next_timesync_recovery_frame has been reached revert back to the normal throttle speed
  s32 current_frame = CurrentFrame();
  if (s_target_speed != 1.0f && current_frame >= s_next_timesync_recovery_frame)
  {
    s_target_speed = 1.0f;
    UpdateThrottlePeriod();

    Log_VerbosePrintf("TimeSync Recovery: frame %d, target speed %.4f%%", current_frame, s_target_speed * 100.0f);
  }

  s_next_frame_time += s_frame_period;

  // If we're running too slow, advance the next frame time based on the time we lost. Effectively skips
  // running those frames at the intended time, because otherwise if we pause in the debugger, we'll run
  // hundreds of frames when we resume.
  Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
  if (current_time > s_next_frame_time)
  {
    const Common::Timer::Value diff = static_cast<s64>(current_time) - static_cast<s64>(s_next_frame_time);
    s_next_frame_time += (diff / s_frame_period) * s_frame_period;
    PollEnet(0);
    return;
  }

  // Poll at 1ms throughout the sleep.
  // We need to send our ping requests through.
  const Common::Timer::Value sleep_period = Common::Timer::ConvertMillisecondsToValue(2);
  while (IsActive())
  {
    current_time = Common::Timer::GetCurrentValue();
    if (current_time >= s_next_frame_time)
      break;

    PollEnet(std::min(current_time + sleep_period, s_next_frame_time));
  }
}

void Netplay::GenerateChecksumForFrame(int* checksum, int frame, unsigned char* buffer, int buffer_size)
{
  const u32 sliding_window_size = 4096 * 4; // 4 pages.
  const u32 num_group_of_pages = buffer_size / sliding_window_size;
  const u32 start_position = (frame % num_group_of_pages) * sliding_window_size;
  *checksum = XXH32(buffer + start_position, sliding_window_size, frame);
  // Log_VerbosePrintf("Netplay Checksum: f:%d wf:%d c:%u", frame, frame % num_group_of_pages, *checksum);
}

void Netplay::AdvanceFrame()
{
  ggpo_advance_frame(s_ggpo, 0);
}

void Netplay::RunFrame()
{
  PollEnet(0);

  if (!s_ggpo)
    return;
  // housekeeping
  ggpo_idle(s_ggpo);
  // run game
  auto result = GGPO_OK;
  int disconnect_flags = 0;
  Netplay::Input inputs[2] = {};
  // add local input
  if (s_local_handle != GGPO_INVALID_HANDLE)
  {
    auto inp = ReadLocalInput();
    result = AddLocalInput(inp);
  }
  // advance game
  if (GGPO_SUCCEEDED(result))
  {
    result = SyncInput(inputs, &disconnect_flags);
    if (GGPO_SUCCEEDED(result))
    {
      // enable again when rolling back done
      SPU::SetAudioOutputMuted(false);
      NetplayAdvanceFrame(inputs, disconnect_flags);
    }
  }
}

s32 Netplay::CurrentFrame()
{
  s32 current = -1;
  ggpo_get_current_frame(s_ggpo, current);
  return current;
}

void Netplay::CollectInput(u32 slot, u32 bind, float value)
{
  s_net_input[slot][bind] = value;
}

Netplay::Input Netplay::ReadLocalInput()
{
  // get controller data of the first controller (0 internally)
  Netplay::Input inp{0};
  for (u32 i = 0; i < (u32)DigitalController::Button::Count; i++)
  {
    if (s_net_input[0][i] >= 0.25f)
      inp.button_data |= 1 << i;
  }
  return inp;
}

void Netplay::SendChatMessage(const std::string_view& msg)
{
  if (msg.empty())
    return;

  auto pkt = NewControlPacket<ChatMessage>(sizeof(ChatMessage) + static_cast<u32>(msg.length()));
  std::memcpy(pkt.pkt->data + sizeof(ChatMessage), msg.data(), msg.length());
  SendControlPacketToAll(pkt);

  // add own netplay message locally to netplay messages
  ShowChatMessage(s_player_id, msg);
}

GGPOErrorCode Netplay::SyncInput(Netplay::Input inputs[2], int* disconnect_flags)
{
  return ggpo_synchronize_input(s_ggpo, inputs, sizeof(Netplay::Input) * 2, disconnect_flags);
}

GGPOErrorCode Netplay::AddLocalInput(Netplay::Input input)
{
  return ggpo_add_local_input(s_ggpo, s_local_handle, &input, sizeof(Netplay::Input));
}

s32 Netplay::GetPing()
{
  const int handle = s_local_handle == 1 ? 2 : 1;
  ggpo_get_network_stats(s_ggpo, handle, &s_last_net_stats);
  return s_last_net_stats.network.ping;
}

u32 Netplay::GetMaxPrediction()
{
  return MAX_ROLLBACK_FRAMES;
}

void Netplay::SetInputs(Netplay::Input inputs[2])
{
  for (u32 i = 0; i < 2; i++)
  {
    auto cont = Pad::GetController(i);
    std::bitset<sizeof(u32) * 8> button_bits(inputs[i].button_data);
    for (u32 j = 0; j < (u32)DigitalController::Button::Count; j++)
      cont->SetBindState(j, button_bits.test(j) ? 1.0f : 0.0f);
  }
}

void Netplay::TestNetplaySession(s32 local_handle, u16 local_port, const std::string& remote_addr, u16 remote_port,
                                 s32 input_delay, std::string game_path)
{
  const bool is_hosting = (local_handle == 1);
  if (!CreateSystem(std::move(game_path), is_hosting))
  {
    Log_ErrorPrintf("Failed to create system.");
    return;
  }

  // create session
  std::string nickname = fmt::format("NICKNAME{}", local_handle);
  if (!Netplay::Start(is_hosting, std::move(nickname), remote_addr, is_hosting ? local_port : remote_port, input_delay))
  {
    // this'll call back to us to shut everything netplay-related down
    Log_ErrorPrint("Failed to Create Netplay Session!");
    System::ShutdownSystem(false);
  }
}

bool Netplay::CreateSession(std::string nickname, s32 port, s32 max_players, std::string password)
{
  // TODO: Password

  const s32 input_delay = 1;

  if (!Netplay::Start(true, std::move(nickname), std::string(), port, input_delay))
  {
    CloseSession();
    return false;
  }
  else if (IsHost())
  {
    // Load savestate if available and only when you are the host.
    // the other peers will get state from the host
    auto save_path = fmt::format("{}\\netplay\\{}.sav", EmuFolders::SaveStates, System::GetRunningSerial());
    System::LoadState(save_path.c_str());
  }

  return true;
}

bool Netplay::JoinSession(std::string nickname, const std::string& hostname, s32 port, std::string password)
{
  // TODO: Password

  const s32 input_delay = 1;

  if (!Netplay::Start(false, std::move(nickname), hostname, port, input_delay))
  {
    CloseSession();
    return false;
  }

  return true;
}

void Netplay::NetplayAdvanceFrame(Netplay::Input inputs[], int disconnect_flags)
{
  Netplay::SetInputs(inputs);
  System::RunFrame();
  Netplay::AdvanceFrame();
}

void Netplay::ExecuteNetplay()
{
  // TODO: Fix this hackery to get out of the CPU loop...
  System::SetState(System::State::Running);

  while (s_state != SessionState::Inactive)
  {
    switch (s_state)
    {
      case SessionState::Connecting:
      {
        UpdateConnectingState();
        continue;
      }

      case SessionState::Resetting:
      {
        UpdateResetState();
        continue;
      }

      case SessionState::Running:
      {
        Netplay::RunFrame();

        // this can shut us down
        Host::PumpMessagesOnCPUThread();
        if (!System::IsValid())
          break;

        System::PresentFrame();
        System::UpdatePerformanceCounters();

        Throttle();
        continue;
      }

      case SessionState::ClosingSession:
      {
        CloseSession();
        break;
      }

      default:
      case SessionState::Initializing:
      case SessionState::Inactive:
      {
        // shouldn't be here
        Panic("Invalid state");
        continue;
      }
    }
  }
}

bool Netplay::NpAdvFrameCb(void* ctx, int flags)
{
  Netplay::Input inputs[2] = {};
  int disconnect_flags;
  Netplay::SyncInput(inputs, &disconnect_flags);
  NetplayAdvanceFrame(inputs, disconnect_flags);
  return true;
}

bool Netplay::NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame)
{
  SaveStateBuffer our_buffer;
  if (s_save_buffer_pool.empty())
  {
    our_buffer = std::make_unique<System::MemorySaveState>();
  }
  else
  {
    our_buffer = std::move(s_save_buffer_pool.front());
    s_save_buffer_pool.pop_front();
  }

  if (!System::SaveMemoryState(our_buffer.get()))
  {
    s_save_buffer_pool.push_front(std::move(our_buffer));
    return false;
  }

  // desync detection
  const u32 state_size = static_cast<u32>(our_buffer.get()->state_stream.get()->GetPosition());
  unsigned char* state = reinterpret_cast<unsigned char*>(our_buffer.get()->state_stream.get()->GetMemoryPointer());
  GenerateChecksumForFrame(checksum, frame, state, state_size);

#if 0
  if (frame > 100 && frame < 150 && s_num_players > 1)
  {
    std::string filename =
      Path::Combine(EmuFolders::Dumps, fmt::format("f{}_c{}_p{}.bin", frame, (u32)*checksum, s_local_handle));
    FileSystem::WriteBinaryFile(filename.c_str(), state, state_size);
  }

  Log_VerbosePrintf("Saved real frame %u as GGPO frame %d CS %u", System::GetFrameNumber(), frame, *checksum);
#endif

  *len = sizeof(System::MemorySaveState);
  *buffer = reinterpret_cast<unsigned char*>(our_buffer.release());

  return true;
}

bool Netplay::NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load)
{
  // Disable Audio For upcoming rollback
  SPU::SetAudioOutputMuted(true);

  const u32 prev_frame = System::GetFrameNumber();
  if (!System::LoadMemoryState(*reinterpret_cast<const System::MemorySaveState*>(buffer)))
    return false;

#if 0
  Log_VerbosePrintf("Loaded real frame %u from GGPO frame %d [prev %u]", System::GetFrameNumber(), frame_to_load,
                    prev_frame);
#endif

  return true;
}

void Netplay::NpFreeBuffCb(void* ctx, void* buffer, int frame)
{
  // Log_VerbosePrintf("Reuse Buffer: %d", frame);
  SaveStateBuffer our_buffer(reinterpret_cast<System::MemorySaveState*>(buffer));
  s_save_buffer_pool.push_back(std::move(our_buffer));
}

bool Netplay::NpOnEventCb(void* ctx, GGPOEvent* ev)
{
  switch (ev->code)
  {
    case GGPOEventCode::GGPO_EVENTCODE_CONNECTED_TO_PEER:
      Log_InfoPrintf("GGPO connected to player: %d", ev->u.connected.player);
      break;
    case GGPOEventCode::GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
      Log_InfoPrintf("GGPO synchronizing with player %d: %d/%d", ev->u.synchronizing.player, ev->u.synchronizing.count,
                     ev->u.synchronizing.total);
      break;
    case GGPOEventCode::GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
      Log_InfoPrintf("GGPO synchronized with player: %d", ev->u.synchronized.player);
      break;
    case GGPOEventCode::GGPO_EVENTCODE_RUNNING:
      Log_InfoPrintf("GGPO running");
      break;
    case GGPOEventCode::GGPO_EVENTCODE_TIMESYNC:
      HandleTimeSyncEvent(ev->u.timesync.frames_ahead, ev->u.timesync.timeSyncPeriodInFrames);
      break;
    case GGPOEventCode::GGPO_EVENTCODE_DESYNC:
      Host::OnNetplayMessage(fmt::format("Desync Detected: Current Frame: {}, Desync Frame: {}, Diff: {}, L:{}, R:{}",
                                         CurrentFrame(), ev->u.desync.nFrameOfDesync,
                                         CurrentFrame() - ev->u.desync.nFrameOfDesync, ev->u.desync.ourCheckSum,
                                         ev->u.desync.remoteChecksum));
      break;
    default:
      Log_ErrorPrintf("Netplay Event Code: %d", static_cast<int>(ev->code));
      break;
  }

  return true;
}
