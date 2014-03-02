#pragma once

#include "r3dNetwork.h"
#include "multiplayer/P2PMessages.h"

#include "../../ServerNetPackets/NetPacketsGameInfo.h"
#include "Backend/ServerUserProfile.h"

class GameObject;
class obj_ServerPlayer;
class ServerWeapon;

__forceinline obj_ServerPlayer* IsServerPlayer(const GameObject* obj)
{
	if(obj == NULL) return NULL;
	if(obj->isObjType(OBJTYPE_Human)) return (obj_ServerPlayer*)obj;
	else return NULL;
}

class ServerGameLogic : public r3dNetCallback
{
  protected:
	r3dNetwork	g_net;
	// r3dNetCallback virtuals
virtual	void		OnNetPeerConnected(DWORD peerId);
virtual	void		OnNetPeerDisconnected(DWORD peerId);
virtual	void		OnNetData(DWORD peerId, const r3dNetPacketHeader* packetData, int packetSize);

  public:
	enum { MAX_NUM_PLAYERS = 128 };

	GBGameInfo	ginfo_;	// game info
	uint32_t	creatorID_; // customerID of player who created game, 0 - if permanent game
	
	enum { MAX_PEERS_COUNT = 255 }; // (double size to include temporary connects) - keep under BYTE size 
	// peer-to-player array
	enum peerStatus_e 
	{
	  PEER_FREE,
	  PEER_CONNECTED,
	  PEER_VALIDATED1,	// version validated
	  PEER_LOADING,
	  PEER_PLAYING,		// 
	};
	
	struct peerInfo_s 
	{
	  peerStatus_e	status_;
	  float		startTime;	// status start time
	  void		SetStatus(peerStatus_e status) {
	    status_   = status;
	    startTime = r3dGetTime();
	  }

	  int		playerIdx;
	  obj_ServerPlayer* player;
	  
	  // user id and it profile
	  DWORD		CustomerID;
	  DWORD		SessionID;
	  DWORD		CharID;
	  DWORD		startGameAns; // PKT_S2C_StartGameAns_s::RES_ codes
	  CServerUserProfile temp_profile;

	  // security report stuff (active when player is created)
	  float		secRepRecvTime;	// last time security report was received
	  float		secRepGameTime;	// last value of reported game time
	  float		secRepRecvAccum;
	  
	  float		lastPacketTime;
	  int		lastPacketId;
	  
	  void Clear()
	  {
	    status_     = PEER_FREE;
	    startTime   = r3dGetTime();
	    
	    playerIdx   = -1;
	    player      = NULL;

	    startGameAns= 0;
	    CustomerID  = 0;
	    CharID      = 0;
	  }
	};

	peerInfo_s	peers_[MAX_PEERS_COUNT];	// peer to player table (double size to include temporary connects)
	peerInfo_s&	GetPeer(DWORD peerId)
	{
          r3d_assert(peerId < MAX_PEERS_COUNT);
          return peers_[peerId];
	}

	void		DisconnectPeer(DWORD peerId, bool cheat, const char* message, ...);

	// actual player data - based on it ID
	peerInfo_s*	plrToPeer_[MAX_NUM_PLAYERS];	// index to players table
	int		curPlayers_;
	int		maxPlayers_;
	int		curPeersConnected;
	obj_ServerPlayer* CreateNewPlayer(DWORD peerId, const r3dPoint3D& spawnPos, float spawnDir);
	void		DeletePlayer(int playerIdx, obj_ServerPlayer* plr);
	obj_ServerPlayer* GetPlayer(int playerIdx)
	{
	  r3d_assert(playerIdx < MAX_NUM_PLAYERS);
	  if(plrToPeer_[playerIdx] == NULL)
	    return NULL;

	  return plrToPeer_[playerIdx]->player;
	}
	
	DWORD		GetExternalIP()
	{
	    return g_net.firstBindIP_;
	}
	
	bool		CheckForPlayersAround(const r3dPoint3D& pos, float dist);
	void		GetStartSpawnPosition(const wiCharDataFull& loadout, r3dPoint3D* pos, float* dir);
	void		  GetSpawnPositionNewPlayer(const r3dPoint3D& GamePos, r3dPoint3D* pos, float* dir);
	void		  GetSpawnPositionAfterDeath(const r3dPoint3D& GamePos, r3dPoint3D* pos, float* dir);

	r3dPoint3D	AdjustPositionToFloor(const r3dPoint3D& pos);
	
	void		NetRegisterObjectToPeers(GameObject* netObj);		// register any net object visibility to current players
	void		UpdateNetObjVisData(const obj_ServerPlayer* plr);	// update all network object visibility of passed player
	void		  UpdateNetObjVisData(DWORD peerId, GameObject* netObj);
	void		ResetNetObjVisData(const obj_ServerPlayer* plr);
	
	#define DEFINE_PACKET_FUNC(XX) \
	  void On##XX(const XX##_s& n, GameObject* fromObj, DWORD peerId, bool& needPassThru);
	#define IMPL_PACKET_FUNC(CLASS, XX) \
	  void CLASS::On##XX(const XX##_s& n, GameObject* fromObj, DWORD peerId, bool& needPassThru)

	int		ProcessWorldEvent(GameObject* fromObj, DWORD eventId, DWORD peerId, const void* packetData, int packetSize);
	 DEFINE_PACKET_FUNC(PKT_C2S_ValidateConnectingPeer);
	 DEFINE_PACKET_FUNC(PKT_C2S_JoinGameReq);
	 DEFINE_PACKET_FUNC(PKT_C2S_StartGameReq);
	 DEFINE_PACKET_FUNC(PKT_C2S_Temp_Damage);
	 DEFINE_PACKET_FUNC(PKT_C2C_ChatMessage);
	 //ViruZ Group
	 DEFINE_PACKET_FUNC(PKT_S2C_SendGroupInvite);
	 DEFINE_PACKET_FUNC(PKT_S2C_SendGroupAccept);
	 DEFINE_PACKET_FUNC(PKT_S2C_SendGroupNoAccept);
	 DEFINE_PACKET_FUNC(PKT_S2C_SendGroupData);
	 DEFINE_PACKET_FUNC(PKT_S2C_ReceivedGroupInvite);
	 DEFINE_PACKET_FUNC(PKT_S2C_GetCustomerID);
	//ViruZ Group//
	 DEFINE_PACKET_FUNC(PKT_C2S_DataUpdateReq);
	 DEFINE_PACKET_FUNC(PKT_C2S_SecurityRep);
	 DEFINE_PACKET_FUNC(PKT_C2S_UseNetObject);
	 DEFINE_PACKET_FUNC(PKT_C2S_CreateNote);
	 DEFINE_PACKET_FUNC(PKT_C2S_TEST_SpawnDummyReq);
	 DEFINE_PACKET_FUNC(PKT_C2S_Admin_PlayerKick);
	 DEFINE_PACKET_FUNC(PKT_C2S_Admin_GiveItem);
	 DEFINE_PACKET_FUNC(PKT_C2S_DBG_LogMessage);
	
	void		OnPKT_C2S_ScreenshotData(DWORD peerId, const int size, const char* data);
	 
	void		ValidateMove(GameObject* fromObj, const void* packetData, int packetSize);
	
	int		ProcessChatCommand(obj_ServerPlayer* plr, const char* cmd);
	int       Cmd_GodMode(obj_ServerPlayer* plr, const char* cmd);
	int		  Cmd_Teleport(obj_ServerPlayer* plr, const char* cmd);
	int		  Cmd_GiveItem(obj_ServerPlayer* plr, const char* cmd);
	int		  Cmd_SetVitals(obj_ServerPlayer* plr, const char* cmd);
			  
	int       Cmd_TPMTP(obj_ServerPlayer* plr, const char* cmd);
	int       Cmd_TPPTM(obj_ServerPlayer* plr, const char* cmd);
	int       Cmd_Kill(obj_ServerPlayer* plr, const char* cmd);
	int       Cmd_Kick(obj_ServerPlayer* plr, const char* cmd);
	//ViruZ Group
	obj_ServerPlayer* FindPlayer(char* Name);
	obj_ServerPlayer* FindPlayerCustom(int CustomerID);
	//ViruZ Group//
	void		RelayPacket(DWORD peerId, const GameObject* from, const DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered);
	void		p2pBroadcastToActive(const GameObject* from, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered = true);
	void		p2pBroadcastToAll(const GameObject* from, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered = true);
	void		p2pSendToPeer(DWORD peerId, const GameObject* from, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered = true);
	void		p2pSendRawToPeer(DWORD peerId, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered = true);

	bool		CanDamageThisObject(const GameObject* targetObj);
	void		ApplyDamage(GameObject* sourceObj, GameObject* targetObj, const r3dPoint3D& dmgPos, float damage, bool force_damage, STORE_CATEGORIES damageSource);
	bool		ApplyDamageToPlayer(GameObject* sourceObj, obj_ServerPlayer* targetPlr, const r3dPoint3D& dmgPos, float damage, int bodyBone, int bodyPart, bool force_damage, STORE_CATEGORIES damageSource, const int airState = 0 );  
	bool		ApplyDamageToZombie(GameObject* sourceObj, GameObject* targetZombie, const r3dPoint3D& dmgPos, float damage, int bodyBone, int bodyPart, bool force_damage, STORE_CATEGORIES damageSource);  
	void		DoKillPlayer(GameObject* sourceObj, obj_ServerPlayer* targetPlr, STORE_CATEGORIES weaponCat, bool forced_by_server=false, bool fromPlayerInAir = false, bool targetPlayerInAir = false );
	
	void		InformZombiesAboutSound(const obj_ServerPlayer* plr, const ServerWeapon* wpn);

	void		AddPlayerReward(obj_ServerPlayer* plr, EPlayerRewardID rewardID);
	wiStatsTracking	  GetRewardData(obj_ServerPlayer* plr, EPlayerRewardID rewardID);
	void		AddDirectPlayerReward(obj_ServerPlayer* plr, const wiStatsTracking& in_rwd, const char* rewardName);

	//
	// async api funcs.
	//
	void		ApiPlayerUpdateChar(obj_ServerPlayer* plr, bool disconnectAfter = false);
	void		ApiPlayerLeftGame(const obj_ServerPlayer* plr);

	// security stuff
	void		CheckClientsSecurity();
	void		LogInfo(DWORD peerId, const char* msg, const char* fmt = "", ...);
	void		LogCheat(DWORD peerId, int LogID, int disconnect, const char* msg, const char* fmt = "", ...);
	
	bool		gameFinished_;
	int		weaponDataUpdates_;	// number of times weapon data was updated

	float		gameStartTime_;
	__int64		GetUtcGameTime();
	void		SendWeaponsInfoToPlayer(DWORD peerId);
	
	// data size for each logical packet ids
	__int64		netRecvPktSize[256];
	__int64		netSentPktSize[256];
	void		DumpPacketStatistics();

	DWORD		net_lastFreeId;
	DWORD		net_mapLoaded_LastNetID; // to make sure that client has identit map as server
	DWORD		GetFreeNetId()
	{
		//if(net_lastFreeId > 0xFFFFFFFF) //padrao 0xFFFF
		//	r3dError("net_lastFreeId overflow, make it reuse!");
		return net_lastFreeId++;
	}
	
  public:
	ServerGameLogic();
	virtual ~ServerGameLogic();

	void		Init(const GBGameInfo& ginfo, uint32_t creatorID);
	void		CreateHost(int port);
	void		OnGameStart();

	void		Disconnect();

	void		Tick();

	void		SendGameClose();
	
	struct WeaponStats_s
	{
	  uint32_t ItemID;
	  int	ShotsFired;
	  int	ShotsHits;
	  int	Kills;
	  
	  WeaponStats_s()
	  {
		ItemID = 0;
		ShotsFired = 0;
		ShotsHits = 0;
		Kills = 0;
	  }
	};
	std::vector<WeaponStats_s> weaponStats_;
	void		TrackWeaponUsage(uint32_t ItemID, int ShotsFired, int ShotsHits, int Kills);
};

extern	ServerGameLogic	gServerLogic;
