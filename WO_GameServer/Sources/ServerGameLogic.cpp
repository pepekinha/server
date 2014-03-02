#include "r3dPCH.h"
#include "r3d.h"
#include "GameLevel.h"

#include "ServerGameLogic.h"
#include "MasterServerLogic.h"
#include "GameObjects/ObjManag.h"

#include "ObjectsCode/obj_ServerPlayer.h"
#include "ObjectsCode/obj_ServerLightMesh.h"
#include "ObjectsCode/sobj_SpawnedItem.h"
#include "ObjectsCode/sobj_DroppedItem.h"
#include "ObjectsCode/sobj_Note.h"
#include "ObjectsCode/sobj_Grave.h"//Gravestone
#include "ObjectsCode/Zombies/sobj_Zombie.h"
#include "ObjectsCode/obj_ServerPlayerSpawnPoint.h"
#include "ObjectsCode/obj_ServerBarricade.h"

#include "../EclipseStudio/Sources/GameCode/UserProfile.h"
#include "../EclipseStudio/Sources/ObjectsCode/weapons/WeaponArmory.h"

#include "ServerWeapons/ServerWeapon.h"

#include "AsyncFuncs.h"
#include "Async_Notes.h"
#include "NetworkHelper.h"

ServerGameLogic	gServerLogic;

CVAR_FLOAT(	_glm_SpawnRadius,	 1.0f, "");

extern	__int64 cfg_sessionId;

#include "../EclipseStudio/Sources/Gameplay_Params.h"
	CGamePlayParams		GPP_Data;
	DWORD			GPP_Seed = GetTickCount();	// seed used to xor CRC of gpp_data



static bool IsNullTerminated(const char* data, int size)
{
  for(int i=0; i<size; i++) {
    if(data[i] == 0)
      return true;
  }

  return false;
}

//
//
//
//
static void preparePacket(const GameObject* from, DefaultPacket* packetData)
{
	r3d_assert(packetData);
	r3d_assert(packetData->EventID >= 0);

	if(from) {
		r3d_assert(from->GetNetworkID());
		//r3d_assert(from->NetworkLocal);

		packetData->FromID = toP2pNetId(from->GetNetworkID());
	} else {
		packetData->FromID = 0; // world event
	}

	return;
}

ServerGameLogic::ServerGameLogic()
{
	maxPlayers_ = 0;
	curPlayers_ = 0;
	curPeersConnected = 0;

	// init index to players table
	for(int i=0; i<MAX_NUM_PLAYERS; i++) {
		plrToPeer_[i] = NULL;
	}

	// init peer to player table
	for(int i=0; i<MAX_PEERS_COUNT; i++) {
		peers_[i].Clear();
	}
	
	memset(&netRecvPktSize, 0, sizeof(netRecvPktSize));
	memset(&netSentPktSize, 0, sizeof(netSentPktSize));

	net_lastFreeId    = NETID_OBJECTS_START;
	net_mapLoaded_LastNetID = 0;
	
	weaponStats_.reserve(128);
}

ServerGameLogic::~ServerGameLogic()
{
	SAFE_DELETE(g_AsyncApiMgr);
}

void ServerGameLogic::Init(const GBGameInfo& ginfo, uint32_t creatorID)
{
	r3dOutToLog("Game: Initializing with %d players\n", ginfo.maxPlayers); CLOG_INDENT;
	r3d_assert(curPlayers_ == 0);
	r3d_assert(curPeersConnected == 0);

	creatorID_	= creatorID;
	ginfo_      = ginfo;
	maxPlayers_ = ginfo.maxPlayers;
	curPlayers_ = 0;
	curPeersConnected = 0;

	// init game time
	gameStartTime_     = r3dGetTime();
	
	__int64 utcTime = GetUtcGameTime();
	struct tm* tm = _gmtime64(&utcTime);
	r3d_assert(tm);

	char buf[128];
	asctime_s(buf, sizeof(buf), tm);
	r3dOutToLog("Server time is %s", buf);
	
	weaponDataUpdates_ = 0;

	g_AsyncApiMgr = new CAsyncApiMgr();

	return;
}

void ServerGameLogic::CreateHost(int port)
{
	r3dOutToLog("Starting server on port %d\n", port);

	g_net.Initialize(this, "p2pNet");
	g_net.CreateHost(port, MAX_PEERS_COUNT);
	//g_net.dumpStats_ = 2;

	return;
}

void ServerGameLogic::Disconnect()
{
	r3dOutToLog("Disconnect server\n");
	g_net.Deinitialize();

	return;
}

void ServerGameLogic::OnGameStart()
{
	/*
	if(1)
	{
		r3dOutToLog("Main World objects\n"); CLOG_INDENT;
		for(GameObject* obj=GameWorld().GetFirstObject(); obj; obj=GameWorld().GetNextObject(obj))
		{
			if(obj->isActive()) r3dOutToLog("obj: %s %s\n", obj->Class->Name.c_str(), obj->Name.c_str());
		}
	}

	if(1)
	{
		extern ObjectManager ServerDummyWorld;
		r3dOutToLog("Temporary World objects\n"); CLOG_INDENT;
		for(GameObject* obj=ServerDummyWorld.GetFirstObject(); obj; obj=ServerDummyWorld.GetNextObject(obj))
		{
			if(obj->isActive()) r3dOutToLog("obj: %s %s\n", obj->Class->Name.c_str(), obj->Name.c_str());
		}
	}*/


	// record last net id
	net_mapLoaded_LastNetID = gServerLogic.net_lastFreeId;
	r3dOutToLog("net_mapLoaded_LastNetID: %d\n", net_mapLoaded_LastNetID);
	
	// start getting server notes
	CJobGetServerNotes* job = new CJobGetServerNotes();
	job->GameServerId = ginfo_.gameServerId;
	g_AsyncApiMgr->AddJob(job);
}

void ServerGameLogic::CheckClientsSecurity()
{
  const float PEER_CHECK_DELAY = 0.2f;	// do checks every N seconds
  const float IDLE_PEERS_DELAY = 5.0f;	// peer have this N seconds to validate itself
  const float SECREP1_DELAY    = PKT_C2S_SecurityRep_s::REPORT_PERIOD*4; // x4 time of client reporting time (15sec) to receive security report

  static float nextCheck = -1;
  const float curTime = r3dGetTime();
  if(curTime < nextCheck)
    return;
  nextCheck = curTime + PEER_CHECK_DELAY;
  
  for(int peerId=0; peerId<MAX_PEERS_COUNT; peerId++) 
  {
    const peerInfo_s& peer = peers_[peerId];
    if(peer.status_ == PEER_FREE)
      continue;
      
    // check againts not validated peers
    if(peer.status_ == PEER_CONNECTED)
    {
      if(curTime < peer.startTime + IDLE_PEERS_DELAY)
        continue;
      
      DisconnectPeer(peerId, false, "no validation, last:%f/%d", peer.lastPacketTime, peer.lastPacketId);
      continue;
    }
    
    // check for receiveing security report
    if(peer.player != NULL)
    {
      if(curTime > peer.secRepRecvTime + SECREP1_DELAY) {
        DisconnectPeer(peerId, false, "no SecRep, last:%f/%d", peer.lastPacketTime, peer.lastPacketId);
        continue;
      }
    }
  }
  
  return;
}

void ServerGameLogic::ApiPlayerUpdateChar(obj_ServerPlayer* plr, bool disconnectAfter)
{
	// force current GameFlags update
	plr->UpdateGameWorldFlags();

	CJobUpdateChar* job = new CJobUpdateChar(plr);
	job->CharData   = *plr->loadout_;
	job->OldData    = plr->savedLoadout_;
	job->Disconnect = disconnectAfter;
	job->GameDollars = plr->profile_.ProfileData.GameDollars;
	// add character play time to update data
	job->CharData.Stats.TimePlayed += (int)(r3dGetTime() - plr->startPlayTime_);
	//job->CharData.GroupID = plr->loadout_->GroupID ; //ViruZ Group save data db
	g_AsyncApiMgr->AddJob(job);

	// replace character saved loadout. if update will fail, we'll disconnect player and keep everything at sync
	plr->savedLoadout_ = job->CharData;
}

void ServerGameLogic::ApiPlayerLeftGame(const obj_ServerPlayer* plr)
{
	CJobUserLeftGame* job = new CJobUserLeftGame(plr);
	job->GameMapId    = ginfo_.mapId;
	job->GameServerId = ginfo_.gameServerId;
	job->TimePlayed   = (int)(r3dGetTime() - plr->startPlayTime_);
	g_AsyncApiMgr->AddJob(job);
}

void ServerGameLogic::LogInfo(DWORD peerId, const char* msg, const char* fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	StringCbVPrintfA(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	
	LogCheat(peerId, 0, false, msg, buf);
}

void ServerGameLogic::LogCheat(DWORD peerId, int LogID, int disconnect, const char* msg, const char* fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	StringCbVPrintfA(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	const peerInfo_s& peer = GetPeer(peerId);
	DWORD IP = net_->GetPeerIp(peerId);

	extern int cfg_uploadLogs;
	if(/*cfg_uploadLogs ||*/ (LogID > 0)) // always upload cheats
	{
		CJobAddLogInfo* job = new CJobAddLogInfo();
		job->CheatID    = LogID;
		job->CustomerID = peer.CustomerID;
		job->CharID     = peer.CharID;
		job->IP         = IP;
		job->GameServerId = ginfo_.gameServerId;
		r3dscpy(job->Gamertag, peer.temp_profile.ProfileData.ArmorySlots[0].Gamertag);
		r3dscpy(job->Msg, msg);
		r3dscpy(job->Data, buf);
		g_AsyncApiMgr->AddJob(job);
  	}
  	
  	const char* screenname = "<NOT_CONNECTED>";
  	if(peer.status_ == PEER_PLAYING)
  		screenname = peer.temp_profile.ProfileData.ArmorySlots[0].Gamertag;

	r3dOutToLog("%s: peer%02d, r:%s %s, CID:%d [%s], ip:%s\n", 
		LogID > 0 ? "!!! cheat" : "LogInfo",
		peerId, 
		msg, buf,
		peer.CustomerID, screenname,
		inet_ntoa(*(in_addr*)&IP));

	if(disconnect && peer.player && !peer.player->profile_.ProfileData.isDevAccount)
	{
		// tell client he's cheating.
		// ptumik: no need to make it easier to hack
		//PKT_S2C_CheatWarning_s n2;
		//n2.cheatId = (BYTE)cheatId;
		//p2pSendRawToPeer(peerId, NULL, &n2, sizeof(n2));

		net_->DisconnectPeer(peerId);
		// fire up disconnect event manually, enet might skip if if other peer disconnect as well
		OnNetPeerDisconnected(peerId);
	}
  
	return;
}

void ServerGameLogic::DisconnectPeer(DWORD peerId, bool cheat, const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  StringCbVPrintfA(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  LogCheat(peerId, cheat ? 99 : 0, false, "DisconnectPeer", buf);

  net_->DisconnectPeer(peerId);
  
  // fire up disconnect event manually, enet might skip if if other peer disconnect as well
  OnNetPeerDisconnected(peerId);
}

void ServerGameLogic::OnNetPeerConnected(DWORD peerId)
{
	// too many connections, do not allow more connects
	if(peerId >= MAX_PEERS_COUNT)
	{
		r3dOutToLog("!!! peer%02d over MAX_PEERS_COUNT connected\n", peerId);
		net_->DisconnectPeer(peerId);
		return;
	}

	if(gameFinished_)
	{
		r3dOutToLog("peer connected while game is finished\n");
		return;
	}
		
	r3d_assert(maxPlayers_ > 0);

	//r3dOutToLog("peer%02d connected\n", peerId); CLOG_INDENT;

	peerInfo_s& peer = GetPeer(peerId);
	peer.SetStatus(PEER_CONNECTED);

	curPeersConnected++;
	return;
}

void ServerGameLogic::OnNetPeerDisconnected(DWORD peerId)
{
	// too many connections, do not do anything
	if(peerId >= MAX_PEERS_COUNT)
		return;

	//r3dOutToLog("peer%02d disconnected\n", peerId); CLOG_INDENT;

	peerInfo_s& peer = GetPeer(peerId);

	// debug validation
	switch(peer.status_)
	{
	default: 
		r3dError("!!! Invalid status %d in disconnect !!!", peer.status_);
		break;
	case PEER_FREE:
		break;

	case PEER_CONNECTED:
	case PEER_VALIDATED1:
		r3d_assert(peer.player == NULL);
		r3d_assert(peer.playerIdx == -1);
		break;

	case PEER_LOADING:
		r3d_assert(peer.playerIdx != -1);
		r3d_assert(plrToPeer_[peer.playerIdx] != NULL);
		r3d_assert(peer.player == NULL);
		
		plrToPeer_[peer.playerIdx] = NULL;
		break;

	case PEER_PLAYING:
		r3d_assert(peer.playerIdx != -1);
		r3d_assert(plrToPeer_[peer.playerIdx] != NULL);
		r3d_assert(plrToPeer_[peer.playerIdx] == &peer);
		if(peer.player)
		{
			gMasterServerLogic.RemovePlayer(peer.CustomerID);

			if(peer.player->loadout_->Alive && !peer.player->wasDisconnected_)
			{
				r3dOutToLog("peer%02d player %s is updating his data\n", peerId, peer.player->userName);
				ApiPlayerUpdateChar(peer.player);
			}

			ApiPlayerLeftGame(peer.player);
		
			// report to users
			{
				PKT_S2C_PlayerNameLeft_s n;
				n.peerId = (BYTE)peerId;

				// send to all, regardless visibility
				for(int i=0; i<MAX_PEERS_COUNT; i++) {
					if(peers_[i].status_ >= PEER_PLAYING && peers_[i].player && i != peerId) {
						net_->SendToPeer(&n, sizeof(n), i, true);
					}
				}
			}

			DeletePlayer(peer.playerIdx, peer.player);
		}

		plrToPeer_[peer.playerIdx] = NULL;
		break;
	}

	if(peer.status_ != PEER_FREE)
	{
		// OnNetPeerDisconnected can fire multiple times, because of forced disconnect
		curPeersConnected--;
	}

	// clear peer status
	peer.Clear();

	return;
}

void ServerGameLogic::OnNetData(DWORD peerId, const r3dNetPacketHeader* packetData, int packetSize)
{
	// too many connections, do not do anything
	if(peerId >= MAX_PEERS_COUNT)
		return;

	// we can receive late packets from logically disconnected peer.
	peerInfo_s& peer = GetPeer(peerId);
	if(peer.status_ == PEER_FREE)
		return;
		
	if(packetSize < sizeof(DefaultPacket))
	{
		DisconnectPeer(peerId, true, "small packetSize %d", packetSize);
		return;
	}
	const DefaultPacket* evt = static_cast<const DefaultPacket*>(packetData);

	// store last packet data for debug
	peer.lastPacketTime = r3dGetTime();
	peer.lastPacketId   = evt->EventID;
	
	// store received sizes by packets
	if(evt->EventID < 256)
		netRecvPktSize[evt->EventID] += packetSize;

	if(gameFinished_)
	{
		r3dOutToLog("!!! peer%02d got packet %d while game is finished\n", peerId, evt->EventID);
		return;
	}

	GameObject* fromObj = GameWorld().GetNetworkObject(evt->FromID);

	// pass to world even processor first.
	if(ProcessWorldEvent(fromObj, evt->EventID, peerId, packetData, packetSize)) {
		return;
	}

	if(evt->FromID && fromObj == NULL) {
		DisconnectPeer(peerId, true, "bad event %d from not registered object %d", evt->EventID, evt->FromID);
		return;
	}

	if(fromObj) 
	{
		if(IsServerPlayer(fromObj)) 
		{
			// make sure that sender of that packet is same player on server
			if(((obj_ServerPlayer*)fromObj)->peerId_ != peerId) 
			{
				LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Network, false, "PlayerPeer",
					"peerID: %d, player: %d, packetID: %d", 
					peerId, ((obj_ServerPlayer*)fromObj)->peerId_, evt->EventID);
				return;
			}
		}

		if(!fromObj->OnNetReceive(evt->EventID, packetData, packetSize)) 
		{
			LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Network, true, "BadObjectEvent",
				"%d for %s %d", 
				evt->EventID, fromObj->Name.c_str(), fromObj->GetNetworkID());
		}
		return;
	}

	LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Network, true, "BadWorldEvent",
		"event %d from %d, obj:%s", 
		evt->EventID, evt->FromID, fromObj ? fromObj->Name.c_str() : "NONE");
	return;
}

void ServerGameLogic::DoKillPlayer(GameObject* sourceObj, obj_ServerPlayer* targetPlr, STORE_CATEGORIES weaponCat, bool forced_by_server, bool fromPlayerInAir, bool targetPlayerInAir )
{
	r3dOutToLog("%s killed by %s, forced: %d\n", targetPlr->userName, sourceObj->Name.c_str(), (int)forced_by_server);
	
	
	//Gravestone
	 char plr2msg[128] = {0};
     obj_Grave* obj = (obj_Grave*)srv_CreateGameObject("obj_Grave", "obj_Grave", targetPlr->GetPosition());
     obj->SetNetworkID(GetFreeNetId());
     obj->NetworkLocal = true;
 
   // vars
   if(IsServerPlayer(sourceObj))
   {
     obj_ServerPlayer * killedByPlr = ((obj_ServerPlayer*)sourceObj);
    if (targetPlr->profile_.CustomerID == killedByPlr->profile_.CustomerID)
		{
        sprintf(plr2msg, "Commit Suicide");
		} 
	else 
		{
		 sprintf(plr2msg, "KILLED BY %s", killedByPlr->loadout_->Gamertag);
		}
	
   } 
   else if(sourceObj->isObjType(OBJTYPE_Zombie))
		{
         sprintf(plr2msg, "EATEN BY ZOMBIE");
		} 
	else 
		{
          sprintf(plr2msg, "Commit Suicide");
        }
	
   
	obj->m_Note.NoteID     = 0;
	_time64(&obj->m_Note.CreateDate);
	obj->m_Note.plr1   = targetPlr->userName;
	obj->m_Note.plr2 = plr2msg;
	obj->m_Note.ExpireMins = 60;
	obj->OnCreate();
	// Fim gravestone

	// sent kill packet
	{		     
		PKT_S2C_KillPlayer_s n;
		n.targetId = toP2pNetId(targetPlr->GetNetworkID());
		n.killerWeaponCat = (BYTE)weaponCat;
		n.forced_by_server = forced_by_server;
		p2pBroadcastToActive(sourceObj, &n, sizeof(n));
	}
	//MENSAGEM DE KILL PLAYER /*wpn->getConfig()->m_StoreName*/
	{
	//wiCharDataFull& wpn = ServerWeapon::m_Owner;
	//const WeaponConfig* wpn = g_pWeaponArmory->getWeaponConfig(itemID);
	//char weaponanme = wpn->m_StoreName;	
	//char weaponname = g_pWeaponArmory->getWeaponConfig(plr->m_SelectedWeapon)->m_StoreName;
	obj_ServerPlayer * plr = ((obj_ServerPlayer*)sourceObj);
	uint32_t itemID = plr->m_SelectedWeapon;
	const WeaponConfig* wpn = g_pWeaponArmory->getWeaponConfig(itemID);
	const char *weaponname = "wpn->m_StoreNameW";
		
	char message[64] = {0};
	sprintf(message, "%s killed by %s with: %d\n", targetPlr->userName, sourceObj->Name.c_str(), weaponname);
	PKT_C2C_ChatMessage_s n2;
    n2.userFlag = 0;
    n2.msgChannel = 1;
    r3dscpy(n2.msg, message);
    r3dscpy(n2.gamertag, "<system>");
    for(int i=0; i<MAX_PEERS_COUNT; i++)
		{
				if(peers_[i].status_ >= PEER_PLAYING && peers_[i].player) 
				{
					net_->SendToPeer(&n2, sizeof(n2), i, true);
				}
		}
	}
	/*
	if(!forced_by_server && sourceObj != targetPlr) // do not reward suicides
	{
		DropLootBox(targetPlr);
	}*/
		
	targetPlr->DoDeath();

	if(forced_by_server)
		return;
	
	//Perde Reputação quando morre na arena
	/*if (targetPlr->loadout_->GameMapId == GBGameInfo::MAPID_WZ_ViruZ_pvp)
	{
		targetPlr->loadout_->Stats.Reputation -= 2;
		PKT_S2C_SetPlayerReputation_s n;
		n.Reputation = targetPlr->loadout_->Stats.Reputation;
		p2pBroadcastToActive(targetPlr, &n, sizeof(n));
	}*/

	targetPlr->loadout_->Stats.Deaths++;
	
	// Perde kills na arena
	if (targetPlr->loadout_->GameMapId == GBGameInfo::MAPID_WZ_ViruZ_pvp)
	{
		targetPlr->loadout_->Stats.Kills--;// Tira kill quando morre!
	}
	//AddPlayerReward(targetPlr, RWD_Death, "RWD_Death"); //add rewar no player que morre.
	

	// do not count suicide kills
	if(sourceObj == targetPlr)
		return;
	

	if(IsServerPlayer(sourceObj))
	{
		obj_ServerPlayer * fromPlr = ((obj_ServerPlayer*)sourceObj);
		fromPlr->loadout_->Stats.Kills++;

		const int fromPlrOldReputation = fromPlr->loadout_->Stats.Reputation;
	
	if (targetPlr->loadout_->GameMapId != GBGameInfo::MAPID_WZ_ViruZ_pvp)
	 {
		if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Constable &&
			targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Civilian)
		{
			if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Paragon)
			{
				fromPlr->loadout_->Stats.Reputation -= 125;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Vigilante)
			{
				fromPlr->loadout_->Stats.Reputation -= 60;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Guardian)
			{
				fromPlr->loadout_->Stats.Reputation -= 40;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Lawman)
			{
				fromPlr->loadout_->Stats.Reputation -= 15;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Deputy)
			{
				fromPlr->loadout_->Stats.Reputation -= 2;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Constable)
			{
				fromPlr->loadout_->Stats.Reputation -= 1;
			}
		}
		else 
		{			  
			if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Paragon)
			{
				fromPlr->loadout_->Stats.Reputation -= 20;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Vigilante)
			{
				fromPlr->loadout_->Stats.Reputation -= 15;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Guardian)
			{
				fromPlr->loadout_->Stats.Reputation -= 10;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Lawman)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Deputy)
			{
				fromPlr->loadout_->Stats.Reputation -= 3;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Constable)
			{
				fromPlr->loadout_->Stats.Reputation -= 2;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Civilian)
			{
				fromPlr->loadout_->Stats.Reputation -= 1;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Villain)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -20 : 20;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Assassin)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -15 : 15;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Hitman)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -10 : 10;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Bandit)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -4 : 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= -ReputationPoints::Outlaw)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -3 : 3;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -2 : 2;
			}
		}
	}//Arena Reputação
	else 
	{
		if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Constable &&
			targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Civilian)
		{
			if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Paragon)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Vigilante)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Guardian)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Lawman)
			{
				fromPlr->loadout_->Stats.Reputation -= 3;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Deputy)
			{
				fromPlr->loadout_->Stats.Reputation -= 2;
			}
			else if (fromPlr->loadout_->Stats.Reputation >= ReputationPoints::Constable)
			{
				fromPlr->loadout_->Stats.Reputation -= 1;
			}
		}
		else 
		{			  
			if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Paragon)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Vigilante)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Guardian)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Lawman)
			{
				fromPlr->loadout_->Stats.Reputation -= 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Deputy)
			{
				fromPlr->loadout_->Stats.Reputation -= 3;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Constable)
			{
				fromPlr->loadout_->Stats.Reputation -= 2;
			}
			else if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Civilian)
			{
				fromPlr->loadout_->Stats.Reputation -= 1;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Villain)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -4 : 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Assassin)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -4 : 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Hitman)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -4 : 4;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Bandit)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -3 : 3;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= -ReputationPoints::Outlaw)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -2 : 2;
			}
			else if (targetPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug)
			{
				fromPlr->loadout_->Stats.Reputation +=
					fromPlr->loadout_->Stats.Reputation <= ReputationPoints::Thug ? -1 : 1;
			}
		}
	}
	//Fim Reputação arena

		if (targetPlr->loadout_->Stats.Reputation >= ReputationPoints::Civilian)
		{
			fromPlr->loadout_->Stats.KilledSurvivors++;
		}
		else
		{
			fromPlr->loadout_->Stats.KilledBandits++;
		}
		
		PKT_S2C_SetPlayerReputation_s n;
		n.Reputation = fromPlr->loadout_->Stats.Reputation;
		p2pBroadcastToActive(fromPlr, &n, sizeof(n));
		
		
		//XP conta VIP
		if(fromPlr->profile_.ProfileData.AccountType == 5) //KILLXP
		{	
			gServerLogic.AddPlayerReward(fromPlr, RWD_PlayerKill);
		}
	}
	

	else if(sourceObj->isObjType(OBJTYPE_GameplayItem) && sourceObj->Class->Name == "obj_ServerAutoTurret")
	{
		// award kill to owner of the turret
		obj_ServerPlayer* turretOwner = IsServerPlayer(GameWorld().GetObject(sourceObj->ownerID));
		if(turretOwner)
		{
			turretOwner->loadout_->Stats.Kills++;
		}
	}

	return;
}

// make sure this function is the same on client: AI_Player.cpp bool canDamageTarget(const GameObject* obj)
bool ServerGameLogic::CanDamageThisObject(const GameObject* targetObj)
{
    obj_ServerPlayer* plr = (obj_ServerPlayer*)IsServerPlayer(targetObj);
    if(plr && ((plr->profile_.ProfileData.isDevAccount && plr->profile_.ProfileData.isGod) || (plr->loadout_->GameFlags & wiCharDataFull::GAMEFLAG_SpawnProtection)))
    {
        return false;
    }

    if(IsServerPlayer(targetObj))
    {
        return true;
    }
	else if(targetObj->isObjType(OBJTYPE_Zombie))
	{
		return true;
	}
	else if(targetObj->Class->Name == "obj_LightMesh")
	{
		return true;
	}
	else if(targetObj->Class->Name == "obj_ServerBarricade")
	{
		return true;
	}

	return false;
}

void ServerGameLogic::ApplyDamage(GameObject* fromObj, GameObject* targetObj, const r3dPoint3D& dmgPos, float damage, bool force_damage, STORE_CATEGORIES damageSource)
{
	r3d_assert(fromObj);
	r3d_assert(targetObj);
	
	if(IsServerPlayer(targetObj))
	{
		ApplyDamageToPlayer(fromObj, (obj_ServerPlayer*)targetObj, dmgPos, damage, -1, 0, force_damage, damageSource);
		return;
	}
	else if(targetObj->isObjType(OBJTYPE_Zombie))
	{
		ApplyDamageToZombie(fromObj, targetObj, dmgPos, damage, -1, 0, force_damage, damageSource);
		return;
	}
	else if(targetObj->Class->Name == "obj_LightMesh")
	{
		obj_ServerLightMesh* lightM = (obj_ServerLightMesh*)targetObj;
		if(lightM->bLightOn)
		{
			lightM->bLightOn = false;
			lightM->SyncLightStatus(-1);
		}
		
		return;
	}
	else if(targetObj->Class->Name == "obj_ServerBarricade")
	{
		obj_ServerBarricade* shield = (obj_ServerBarricade*)targetObj;
		shield->DoDamage(damage);
		return;
	}

	r3dOutToLog("!!! error !!! was trying to damage object %s [%s]\n", targetObj->Name.c_str(), targetObj->Class->Name.c_str());
}

// return true if hit was registered, false otherwise
// state is grabbed from the dynamics.  [0] is from player in the air, [1] is target player in the air
bool ServerGameLogic::ApplyDamageToPlayer(GameObject* fromObj, obj_ServerPlayer* targetPlr, const r3dPoint3D& dmgPos, float damage, int bodyBone, int bodyPart, bool force_damage, STORE_CATEGORIES damageSource, int airState )
{
	r3d_assert(fromObj);
	r3d_assert(targetPlr);

	if(targetPlr->loadout_->Alive == 0 || targetPlr->profile_.ProfileData.isGod)
		return false;

	if(fromObj->isObjType(OBJTYPE_Human) && ((obj_ServerPlayer*)fromObj)->profile_.ProfileData.isGod)
		return false;
	
	// Cannot damage same groups
	if(IsServerPlayer(fromObj))
	{
		obj_ServerPlayer * fromPlr = ((obj_ServerPlayer*)fromObj);
		if(fromPlr->loadout_->GroupID == targetPlr->loadout_->GroupID && targetPlr->loadout_->GroupID != 0 && fromObj->isObjType(OBJTYPE_Human))
		{
			return false;
		}
	}
	
	// Cannot damage same Clan
	if(IsServerPlayer(fromObj))
	{
		obj_ServerPlayer * fromPlr = ((obj_ServerPlayer*)fromObj);
		if(fromPlr->loadout_->ClanID  == targetPlr->loadout_->ClanID && targetPlr->loadout_->ClanID != 0 && fromObj->isObjType(OBJTYPE_Human))
		{
			return false;
		}
	}

	// can't damage players in safe zones (postbox now act like that)
	if(((targetPlr->loadout_->GameFlags & wiCharDataFull::GAMEFLAG_NearPostBox) || (targetPlr->loadout_->GameFlags & wiCharDataFull::GAMEFLAG_SpawnProtection)) && !force_damage)
		return false;

	//ViruZ Colorado_pve
	if(targetPlr->loadout_->GameMapId == GBGameInfo::MAPID_WZ_Colorado_pve)
        return false;

	damage = targetPlr->ApplyDamage(damage, fromObj, bodyPart, damageSource);

	// send damage packet, originating from the firing dude
	PKT_S2C_Damage_s a;
	a.dmgPos = dmgPos;
	a.targetId = toP2pNetId(targetPlr->GetNetworkID());
	a.damage   = R3D_MIN((BYTE)damage, (BYTE)255);
	a.dmgType = damageSource;
	a.bodyBone = bodyBone;
	p2pBroadcastToActive(fromObj, &a, sizeof(a));

	// check if we killed player
	if(targetPlr->loadout_->Health <= 0) 
	{
		bool fromPlayerInAir = ((airState & 0x1) != 0);
		bool targetPlayerInAir = ((airState & 0x2) != 0);

		DoKillPlayer(fromObj, targetPlr, damageSource, false, fromPlayerInAir, targetPlayerInAir);
	}

	return true;
}

bool ServerGameLogic::ApplyDamageToZombie(GameObject* fromObj, GameObject* targetZombie, const r3dPoint3D& dmgPos, float damage, int bodyBone, int bodyPart, bool force_damage, STORE_CATEGORIES damageSource)
{
	r3d_assert(fromObj);
	r3d_assert(targetZombie && targetZombie->isObjType(OBJTYPE_Zombie));
	
	// relay to zombie logic
	obj_Zombie* z = (obj_Zombie*)targetZombie;
	bool killed = z->ApplyDamage(fromObj, damage, bodyPart, damageSource);
	if(IsServerPlayer(fromObj) && killed)
	{
		IsServerPlayer(fromObj)->loadout_->Stats.KilledZombies++;
	}
	
	return true;
}

void ServerGameLogic::RelayPacket(DWORD peerId, const GameObject* from, const DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered)
{
	if(!from)
	{
		r3dError("RelayPacket !from, event: %d", packetData->EventID);
	}
	const INetworkHelper* nh = const_cast<GameObject*>(from)->GetNetworkHelper();

	for(int i=0; i<MAX_PEERS_COUNT; i++) 
	{
		if(peers_[i].status_ >= PEER_PLAYING && i != peerId) 
		{
			if(!nh->GetVisibility(i))
			{
				continue;
			}

			net_->SendToPeer(packetData, packetSize, i, guaranteedAndOrdered);
			netSentPktSize[packetData->EventID] += packetSize;
		}
	}

	return;
}

void ServerGameLogic::p2pBroadcastToActive(const GameObject* from, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered)
{
	if(!from)
	{
		r3dError("p2pBroadcastToActive !from, event: %d", packetData->EventID);
	}
	const INetworkHelper* nh = const_cast<GameObject*>(from)->GetNetworkHelper();

	preparePacket(from, packetData);

	for(int i=0; i<MAX_PEERS_COUNT; i++) 
	{
		if(peers_[i].status_ >= PEER_PLAYING) 
		{
			if(!nh->GetVisibility(i))
			{
				continue;
			}
			net_->SendToPeer(packetData, packetSize, i, guaranteedAndOrdered);
			netSentPktSize[packetData->EventID] += packetSize;
		}
	}

	return;
}

void ServerGameLogic::p2pBroadcastToAll(const GameObject* from, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered)
{
	preparePacket(from, packetData);

	for(int i=0; i<MAX_PEERS_COUNT; i++) 
	{
		if(peers_[i].status_ >= PEER_VALIDATED1) 
		{
			net_->SendToPeer(packetData, packetSize, i, guaranteedAndOrdered);
			netSentPktSize[packetData->EventID] += packetSize;
		}
	}

	return;
}

void ServerGameLogic::p2pSendToPeer(DWORD peerId, const GameObject* from, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered)
{
	if(!from)
	{
		r3dError("p2pSendToPeer !from, event: %d", packetData->EventID);
	}

	const peerInfo_s& peer = GetPeer(peerId);
	if(peer.status_ >= PEER_PLAYING)
	{
		const INetworkHelper* nh = const_cast<GameObject*>(from)->GetNetworkHelper();
		if(!nh->GetVisibility(peerId))
		{
			return;
		}

		preparePacket(from, packetData);
		net_->SendToPeer(packetData, packetSize, peerId, guaranteedAndOrdered);
		netSentPktSize[packetData->EventID] += packetSize;
	}
}

void ServerGameLogic::p2pSendRawToPeer(DWORD peerId, DefaultPacket* packetData, int packetSize, bool guaranteedAndOrdered)
{
	const peerInfo_s& peer = GetPeer(peerId);
	r3d_assert(peer.status_ != PEER_FREE);

	preparePacket(NULL, packetData);
	net_->SendToPeer(packetData, packetSize, peerId, guaranteedAndOrdered);
	netSentPktSize[packetData->EventID] += packetSize;
}

void ServerGameLogic::InformZombiesAboutSound(const obj_ServerPlayer* plr, const ServerWeapon* wpn)
{
	for(GameObject* obj = GameWorld().GetFirstObject(); obj; obj = GameWorld().GetNextObject(obj))
	{
		if(obj->isObjType(OBJTYPE_Zombie))
			((obj_Zombie*)obj)->SenseWeaponFire(plr, wpn);
	}
}

wiStatsTracking ServerGameLogic::GetRewardData(obj_ServerPlayer* plr, EPlayerRewardID rewardID)
{
	r3d_assert(g_GameRewards);
	const CGameRewards::rwd_s& rwd = g_GameRewards->GetRewardData(rewardID);
	if(!rwd.IsSet)
	{
		LogInfo(plr->peerId_, "GetReward", "%d not set", rewardID);
		return wiStatsTracking();
	}
	
	wiStatsTracking stat;
	stat.RewardID = (int)rewardID;
	stat.GP       = 0;
	
	if(plr->loadout_->Hardcore)
	{
		stat.GD = rwd.GD_HARD;
		stat.XP = rwd.XP_HARD;
	}
	else
	{
		stat.GD = rwd.GD_SOFT;
		stat.XP = rwd.XP_SOFT;
	}
	
	return stat;
}	
	
void ServerGameLogic::AddPlayerReward(obj_ServerPlayer* plr, EPlayerRewardID rewardID)
{
	wiStatsTracking stat = GetRewardData(plr, rewardID);
	if(stat.RewardID == 0)
		return;

	const CGameRewards::rwd_s& rwd = g_GameRewards->GetRewardData(rewardID);
	AddDirectPlayerReward(plr, stat, rwd.Name.c_str());
}

void ServerGameLogic::AddDirectPlayerReward(obj_ServerPlayer* plr, const wiStatsTracking& in_rwd, const char* rewardName)
{
	// add reward to player
	wiStatsTracking rwd2 = plr->AddReward(in_rwd);
	int xp = rwd2.XP;
	int gp = rwd2.GP;
	int gd = rwd2.GD;
	if(xp == 0 && gp == 0 && gd == 0)
		return;
		
	r3dOutToLog("reward: %s got %dxp %dgp %dgd RWD_%s\n", plr->userName, xp, gp, gd, rewardName ? rewardName : "");
  
	// send it to him
	PKT_S2C_AddScore_s n;
	n.ID = (WORD)in_rwd.RewardID;
	n.XP = R3D_CLAMP(xp, -30000, 30000);
	n.GD = (WORD)gd;
	p2pSendToPeer(plr->peerId_, plr, &n, sizeof(n));
  
	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_ValidateConnectingPeer)
{
	// reply back with our version
	PKT_C2S_ValidateConnectingPeer_s n1;
	n1.protocolVersion = P2PNET_VERSION;
	n1.sessionId       = 0;
	p2pSendRawToPeer(peerId, &n1, sizeof(n1));

	if(n.protocolVersion != P2PNET_VERSION) 
	{
		DisconnectPeer(peerId, false, "Version mismatch");
		return;
	}
	extern __int64 cfg_sessionId;
	if(n.sessionId != cfg_sessionId)
	{
		DisconnectPeer(peerId, true, "Wrong sessionId");
		return;
	}

	// switch to Validated state
	peerInfo_s& peer = GetPeer(peerId);
	peer.SetStatus(PEER_VALIDATED1);

	return;
}

obj_ServerPlayer* ServerGameLogic::CreateNewPlayer(DWORD peerId, const r3dPoint3D& spawnPos, float spawnDir)
{
	peerInfo_s& peer = GetPeer(peerId);
	const int playerIdx = peer.playerIdx;

	r3d_assert(playerIdx >= 0 && playerIdx < maxPlayers_);
	r3d_assert(peer.startGameAns == PKT_S2C_StartGameAns_s::RES_Ok);
	
	// store game session id
	peer.temp_profile.ProfileData.ArmorySlots[0].GameServerId = ginfo_.gameServerId;
	peer.temp_profile.ProfileData.ArmorySlots[0].GameMapId    = ginfo_.mapId;

	// create player
	char name[128];
	//sprintf(name, "player%02d", playerIdx);
	sprintf(name, "%s", peer.temp_profile.ProfileData.ArmorySlots[0].Gamertag);
	obj_ServerPlayer* plr = (obj_ServerPlayer*)srv_CreateGameObject("obj_ServerPlayer", name, spawnPos);
	
	// add to peer-to-player table (need to do before player creation, because of network objects visibility check)
	r3d_assert(plrToPeer_[playerIdx] != NULL);
	r3d_assert(plrToPeer_[playerIdx]->player == NULL);
	plrToPeer_[playerIdx]->player = plr;
	// mark that we're active
	peer.player = plr;

	// fill player info	
	plr->m_PlayerRotation = spawnDir;
	plr->peerId_      = peerId;
	plr->SetNetworkID(playerIdx + NETID_PLAYERS_START);
	plr->NetworkLocal = false;
	plr->SetProfile(peer.temp_profile);
	plr->OnCreate();

	// from this point we do expect security report packets
	peer.secRepRecvTime = r3dGetTime();
	peer.secRepGameTime = -1;
	peer.secRepRecvAccum = 0;

	r3d_assert(curPlayers_ < maxPlayers_);
	curPlayers_++;
	
	// report in masterserver
	gMasterServerLogic.AddPlayer(peer.CustomerID);
	
	// report joined player name to all users
	{
		PKT_S2C_PlayerNameJoined_s n;
		n.peerId = (BYTE)peerId;
		r3dscpy(n.gamertag, plr->userName);
		n.flags = 0;
		if(plr->profile_.ProfileData.AccountType == 0) // legend
			n.flags |= 1;
		if(plr->profile_.ProfileData.isDevAccount)
			n.flags |= 2;
		if(plr->profile_.ProfileData.AccountType == 5) // Premium
			n.flags |= 4;
		n.Reputation = plr->loadout_->Stats.Reputation;

		// send to all, regardless visibility, excluding us
		for(int i=0; i<MAX_PEERS_COUNT; i++) {
			if(peers_[i].status_ >= PEER_PLAYING && peers_[i].player && i != peerId) {
				net_->SendToPeer(&n, sizeof(n), i, true);
			}
		}
	}
	
	// report list of current players to user, including us
	{
		for(int i=0; i<MAX_PEERS_COUNT; i++) {
			if(peers_[i].status_ >= PEER_PLAYING && peers_[i].player) {
				PKT_S2C_PlayerNameJoined_s n;
				n.peerId = i;
				r3dscpy(n.gamertag, peers_[i].player->userName);
				n.flags = 0;
				if(peers_[i].player->profile_.ProfileData.AccountType == 0) // legend
					n.flags |= 1;
				if(peers_[i].player->profile_.ProfileData.isDevAccount)
					n.flags |= 2;
				if(peers_[i].player->profile_.ProfileData.AccountType == 5) // Premium
					n.flags |= 4;
				n.Reputation = peers_[i].player->loadout_->Stats.Reputation;

				net_->SendToPeer(&n, sizeof(n), peerId, true);
			}
		}
	}
	
	return plr;
}

void ServerGameLogic::DeletePlayer(int playerIdx, obj_ServerPlayer* plr)
{
	r3d_assert(plr);

	r3d_assert(playerIdx == (plr->GetNetworkID() - NETID_PLAYERS_START));
	r3dOutToLog("DeletePlayer: %s, playerIdx: %d\n", plr->userName, playerIdx);

	ResetNetObjVisData(plr);
	
	// send player destroy
	{
		PKT_S2C_DestroyNetObject_s n;
		n.spawnID = gp2pnetid_t(plr->GetNetworkID());
		p2pBroadcastToActive(plr, &n, sizeof(n));
	}

	// mark for deletion
	plr->setActiveFlag(0);
	//plr->NetworkID = 0;

	r3d_assert(curPlayers_ > 0);
	curPlayers_--;

	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_JoinGameReq)
{
	DWORD ip = net_->GetPeerIp(peerId);
	r3dOutToLog("peer%02d PKT_C2S_JoinGameReq: CID:%d, ip:%s\n", 
		peerId, n.CustomerID, inet_ntoa(*(in_addr*)&ip)); 
	CLOG_INDENT;

	if(n.CustomerID == 0 || n.SessionID == 0 || n.CharID == 0)
	{
		gServerLogic.LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Protocol, true, "JoinGame",
			"%d %d %d", n.CustomerID, n.SessionID, n.CharID);
		return;
	}
	
	// GetFreePlayerSlot
	int playerIdx = -1;
	for(int i=0; i<maxPlayers_; i++) 
	{
		if(plrToPeer_[i] == NULL) 
		{
			playerIdx = i;
			break;
		}
	}

	if(playerIdx == -1)
	{
		PKT_S2C_JoinGameAns_s n;
		n.success   = 0;
		n.playerIdx = 0;
		p2pSendRawToPeer(peerId, &n, sizeof(n));

		DisconnectPeer(peerId, false, "game is full"); //@ investigate why it's happening
		return;
	}

	{ // send answer to peer
		PKT_S2C_JoinGameAns_s n;
		n.success      = 1;
		n.playerIdx    = playerIdx;
		n.gameInfo     = ginfo_;
		n.gameTime     = GetUtcGameTime();

		p2pSendRawToPeer(peerId, &n, sizeof(n));
	}
	
	{  // send game parameters to peer
		PKT_S2C_SetGamePlayParams_s n;
		n.GPP_Data = GPP_Data;
		n.GPP_Seed = GPP_Seed;
		p2pSendRawToPeer(peerId, &n, sizeof(n));
	}

	peerInfo_s& peer = GetPeer(peerId);
	r3d_assert(peer.player == NULL);
	peer.SetStatus(PEER_LOADING);
	peer.playerIdx    = playerIdx;
	peer.CustomerID   = n.CustomerID;
	peer.SessionID    = n.SessionID;
	peer.CharID       = n.CharID;

	// add to players table
	r3d_assert(plrToPeer_[playerIdx] == NULL);
	plrToPeer_[playerIdx] = &peer;
	
	// start thread for profile loading
	g_AsyncApiMgr->AddJob(new CJobProcessUserJoin(peerId));

	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_StartGameReq)
{
	peerInfo_s& peer = GetPeer(peerId);
	r3d_assert(peer.playerIdx != -1);
	r3d_assert(peer.player == NULL);
	r3d_assert(peer.status_ == PEER_LOADING);
	
	r3dOutToLog("peer%02d PKT_C2S_StartGameReq, startGameAns: %d, lastNetID: %d\n", peerId, peer.startGameAns, n.lastNetID); CLOG_INDENT;
	
	if(n.lastNetID != net_mapLoaded_LastNetID)
	{
		PKT_S2C_StartGameAns_s n2;
		n2.result = PKT_S2C_StartGameAns_s::RES_UNSYNC;
		p2pSendRawToPeer(peerId, &n2, sizeof(n2));
		DisconnectPeer(peerId, true, "netID doesn't match %d vs %d", n.lastNetID, net_mapLoaded_LastNetID);
		return;
	}
	
	// check for default values, just in case
	r3d_assert(0 == PKT_S2C_StartGameAns_s::RES_Unactive);
	r3d_assert(1 == PKT_S2C_StartGameAns_s::RES_Ok);
	switch(peer.startGameAns)
	{
		// we have profile, process
		case PKT_S2C_StartGameAns_s::RES_Ok:
			break;
			
		// no profile loaded yet
		case PKT_S2C_StartGameAns_s::RES_Unactive:
		{
			// we give 60sec to finish getting profile per user
			if(r3dGetTime() > (peer.startTime + 60.0f))
			{
				PKT_S2C_StartGameAns_s n;
				n.result = PKT_S2C_StartGameAns_s::RES_Failed;
				p2pSendRawToPeer(peerId, &n, sizeof(n));
				DisconnectPeer(peerId, true, "timeout getting profile data");
			}
			else
			{
				// still pending
				PKT_S2C_StartGameAns_s n;
				n.result = PKT_S2C_StartGameAns_s::RES_Pending;
				p2pSendRawToPeer(peerId, &n, sizeof(n));
			}
			return;
		}
		
		default:
		{
			PKT_S2C_StartGameAns_s n;
			n.result = (BYTE)peer.startGameAns;
			p2pSendRawToPeer(peerId, &n, sizeof(n));
			DisconnectPeer(peerId, true, "StarGameReq: %d", peer.startGameAns);
			return;
		}
	}
	
	// we have player profile, put it in game
	r3d_assert(peer.startGameAns == PKT_S2C_StartGameAns_s::RES_Ok);

  	// we must have only one profile with correct charid
	if(peer.temp_profile.ProfileData.NumSlots != 1 || 
	   peer.temp_profile.ProfileData.ArmorySlots[0].LoadoutID != peer.CharID)
	{
		PKT_S2C_StartGameAns_s n;
		n.result = PKT_S2C_StartGameAns_s::RES_Failed;
		p2pSendRawToPeer(peerId, &n, sizeof(n));

		DisconnectPeer(peerId, true, "CharID mismatch %d vs %d", peer.CharID, peer.temp_profile.ProfileData.ArmorySlots[0].LoadoutID);
		return;
	}

	// and it should be alive.
	wiCharDataFull& loadout = peer.temp_profile.ProfileData.ArmorySlots[0];
	if(loadout.Alive == 0)
	{
		PKT_S2C_StartGameAns_s n;
		n.result = PKT_S2C_StartGameAns_s::RES_Failed;
		p2pSendRawToPeer(peerId, &n, sizeof(n));
			
		DisconnectPeer(peerId, true, "CharID %d is DEAD", peer.CharID);
		return;
	}

	peer.SetStatus(PEER_PLAYING);

	// if by some fucking unknown method you appeared at 0,0,0 - pretend he was dead, so it'll spawn at point
	if(loadout.Alive == 1 && loadout.GameMapId != GBGameInfo::MAPID_ServerTest && loadout.GamePos.Length() < 10)
	{
		LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Data, false, "ZeroSpawn", "%f %f %f", loadout.GamePos.x, loadout.GamePos.y, loadout.GamePos.z);
		loadout.Alive = 2;
	}

	// get spawn position
	r3dPoint3D spawnPos;
	float      spawnDir;
	GetStartSpawnPosition(loadout, &spawnPos, &spawnDir);
	
	// adjust for positions that is under ground because of geometry change //spawn agua
	if(Terrain)
	{
		float y1 = Terrain->GetHeight(spawnPos);
		if(spawnPos.y <= y1)
			spawnPos.y = y1 + 0.1f;
	}

	// create that player
	CreateNewPlayer(peerId, spawnPos, spawnDir);

	// send current weapon info to player
	SendWeaponsInfoToPlayer(peerId);

	// send answer to start game
	{ 
		PKT_S2C_StartGameAns_s n;
		n.result = PKT_S2C_StartGameAns_s::RES_Ok;
		p2pSendRawToPeer(peerId, &n, sizeof(n));
	}

	return;
}

bool ServerGameLogic::CheckForPlayersAround(const r3dPoint3D& pos, float dist)
{
	float distSq = dist * dist;
	for(int i=0; i<ServerGameLogic::MAX_NUM_PLAYERS; i++)
	{
		const obj_ServerPlayer* plr = gServerLogic.GetPlayer(i);
		if(!plr) continue;
		
		if((plr->GetPosition() - pos).LengthSq() < distSq)
			return true;
	}
	
	return false;
}
//antitravel viruz
void ServerGameLogic::GetStartSpawnPosition(const wiCharDataFull& loadout, r3dPoint3D* pos, float* dir)
{
	// if no map assigned yet, or new map, or newly created character (alive == 3)
	if(loadout.GameMapId == 0 || loadout.GameMapId != ginfo_.mapId || loadout.Alive == 3)
	{
		GetSpawnPositionNewPlayer(loadout.GamePos, pos, dir);
		// move spawn pos at radius
		pos->x += u_GetRandom(-_glm_SpawnRadius, _glm_SpawnRadius);
		pos->z += u_GetRandom(-_glm_SpawnRadius, _glm_SpawnRadius);
		//r3dOutToLog("new spawn at position %f %f %f\n", pos->x, pos->y, pos->z);
		return;
	} 
	
	// alive at current map
	if(loadout.GameMapId && (loadout.GameMapId == ginfo_.mapId) && loadout.Alive == 1)
	{
		*pos = loadout.GamePos;
		*dir = loadout.GameDir;
		//r3dOutToLog("alive at position %f %f %f\n", pos->x, pos->y, pos->z);
		return;
	}
	
	// revived (alive == 2) - spawn to closest spawn point
	if(loadout.GameMapId && loadout.Alive == 2)
	{
		GetSpawnPositionAfterDeath(loadout.GamePos, pos, dir);
		// move spawn pos at radius
		pos->x += u_GetRandom(-_glm_SpawnRadius, _glm_SpawnRadius);
		pos->z += u_GetRandom(-_glm_SpawnRadius, _glm_SpawnRadius);
		//r3dOutToLog("revived at position %f %f %f\n", pos->x, pos->y, pos->z);
		return;
	}

	r3dOutToLog("%d %d %d\n", loadout.GameMapId, loadout.Alive, ginfo_.mapId);
	r3d_assert(false && "GetStartSpawnPosition");
}

void ServerGameLogic::GetSpawnPositionNewPlayer(const r3dPoint3D& GamePos, r3dPoint3D* pos, float* dir)
{
	if(gCPMgr.numBaseControlPoints == 0)
	{
		r3dOutToLog("!!!!!!!!!!!! THERE IS NO BASE CONTROL POINTS !!!!!!!\n");
		*pos = r3dPoint3D(0, 0, 0);
		*dir = 0;
		return;
	}

	int idx1 = u_random(gCPMgr.numBaseControlPoints);
	r3d_assert(idx1 < gCPMgr.numBaseControlPoints);
	const BasePlayerSpawnPoint* spawn = gCPMgr.baseControlPoints[idx1];
	spawn->getSpawnPoint(*pos, *dir);
	return;
}

void ServerGameLogic::GetSpawnPositionAfterDeath(const r3dPoint3D& GamePos, r3dPoint3D* pos, float* dir)
{
	if(gCPMgr.numControlPoints_ == 0)
	{
		r3dOutToLog("!!!!!!!!!!!! THERE IS NO CONTROL POINT !!!!!!!\n");
		*pos = r3dPoint3D(0, 0, 0);
		*dir = 0;
		return;
	}
	
	// spawn to closest point
	float minDist = 99999999.0f;
	for(int i=0; i<gCPMgr.numControlPoints_; i++)
	{
		const BasePlayerSpawnPoint* spawn = gCPMgr.controlPoints_[i];
		for(int j=0; j<spawn->m_NumSpawnPoints; j++)
		{
			float dist = (GamePos - spawn->m_SpawnPoints[j].pos).LengthSq();
			if(dist < minDist)
			{
				*pos    = spawn->m_SpawnPoints[j].pos;
				*dir    = spawn->m_SpawnPoints[j].dir;
				minDist = dist;
			}
			
		}
	}

	return;
}

r3dPoint3D ServerGameLogic::AdjustPositionToFloor(const r3dPoint3D& pos)
{
	// do this in a couple of steps. firstly try +0.25, +1, then +5, then +50, then absolute +1000
	PxRaycastHit hit;
	PxSceneQueryFilterData filter(PxFilterData(COLLIDABLE_STATIC_MASK, 0, 0, 0), PxSceneQueryFilterFlag::eSTATIC);

	if(!g_pPhysicsWorld->raycastSingle(PxVec3(pos.x, pos.y+0.25f, pos.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
		if(!g_pPhysicsWorld->raycastSingle(PxVec3(pos.x, pos.y+1.0f, pos.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
			if(!g_pPhysicsWorld->raycastSingle(PxVec3(pos.x, pos.y+5.0f, pos.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
				if(!g_pPhysicsWorld->raycastSingle(PxVec3(pos.x, pos.y+50.0f, pos.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
					if(!g_pPhysicsWorld->raycastSingle(PxVec3(pos.x, 1000.0f, pos.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
					{
						r3dOutToLog("!! there is no floor under %f %f %f\n", pos.x, pos.y, pos.z);
						return pos;
					}
	
	return r3dPoint3D(hit.impact.x, hit.impact.y + 0.001f, hit.impact.z);
}

//
// every server network object must call this function in their OnCreate() - TODO: think about better way to automatize that
//
void ServerGameLogic::NetRegisterObjectToPeers(GameObject* netObj)
{
	r3d_assert(netObj->GetNetworkID());

	// scan for all peers and see if they within distance of this object
	INetworkHelper* nh = netObj->GetNetworkHelper();
	for(int peerId=0; peerId<MAX_PEERS_COUNT; peerId++)
	{
		const peerInfo_s& peer = peers_[peerId];
		if(peer.player == NULL)
			continue;
			
		float dist = (peer.player->GetPosition() - netObj->GetPosition()).LengthSq();
		if(dist < nh->distToCreateSq)
		{
#ifdef _DEBUG			
r3dOutToLog("NETHELPER: %s: on create - entered visibility of network object %d %s\n", peer.player->userName, netObj->GetNetworkID(), netObj->Name.c_str());			
#endif
			r3d_assert(nh->PeerVisStatus[peerId] == 0);
			nh->PeerVisStatus[peerId] = 1;

			int packetSize = 0;
			DefaultPacket* packetData = nh->NetGetCreatePacket(&packetSize);
			if(packetData)
			{
				preparePacket(netObj, packetData);
				net_->SendToPeer(packetData, packetSize, peerId, true);
				netSentPktSize[packetData->EventID] += packetSize;
			}
		}
	}

}

void ServerGameLogic::UpdateNetObjVisData(DWORD peerId, GameObject* netObj)
{
	r3d_assert(netObj->GetNetworkID());
	r3d_assert(!(netObj->ObjFlags & OBJFLAG_JustCreated)); // object must be fully created at this moment
	
	const peerInfo_s& peer = GetPeer(peerId);
	float dist = (peer.player->GetPosition() - netObj->GetPosition()).LengthSq();

	INetworkHelper* nh = netObj->GetNetworkHelper();
	if(nh->PeerVisStatus[peerId] == 0)
	{
		if(dist < nh->distToCreateSq)
		{
#ifdef _DEBUG			
r3dOutToLog("NETHELPER: %s: entered visibility of network object %d %s\n", peer.player->userName, netObj->GetNetworkID(), netObj->Name.c_str());			
#endif
			nh->PeerVisStatus[peerId] = 1;

			int packetSize = 0;
			DefaultPacket* packetData = nh->NetGetCreatePacket(&packetSize);
			if(packetData)
			{
				preparePacket(netObj, packetData);
				net_->SendToPeer(packetData, packetSize, peerId, true);
				netSentPktSize[packetData->EventID] += packetSize;
			}
		}
	}
	else
	{
		// already visible
		if(dist > nh->distToDeleteSq)
		{
#ifdef _DEBUG			
r3dOutToLog("NETHELPER: %s: left visibility of network object %d %s\n", peer.player->userName, netObj->GetNetworkID(), netObj->Name.c_str());			
#endif
			PKT_S2C_DestroyNetObject_s n;
			n.spawnID = toP2pNetId(netObj->GetNetworkID());
			
			// send only to that peer! 
			preparePacket(netObj, &n);
			net_->SendToPeer(&n, sizeof(n), peerId, true);
			netSentPktSize[n.EventID] += sizeof(n);

			nh->PeerVisStatus[peerId] = 0;
		}
	}
}

void ServerGameLogic::UpdateNetObjVisData(const obj_ServerPlayer* plr)
{
	DWORD peerId = plr->peerId_;
	r3d_assert(peers_[peerId].player == plr);

	// scan for all objects and create/destroy them based on distance
	for(GameObject* obj=GameWorld().GetFirstObject(); obj; obj=GameWorld().GetNextObject(obj))
	{
		if(obj->GetNetworkID() == 0)
			continue;
		if(obj->ObjFlags & OBJFLAG_JustCreated)
			continue;
		if(!obj->isActive())
			continue;
			
		UpdateNetObjVisData(peerId, obj);
	}
}

void ServerGameLogic::ResetNetObjVisData(const obj_ServerPlayer* plr)
{
	DWORD peerId       = plr->peerId_;

	// scan for all objects and reset their visibility of player
	for(GameObject* obj=GameWorld().GetFirstObject(); obj; obj=GameWorld().GetNextObject(obj))
	{
		if(obj->GetNetworkID() == 0)
			continue;
			
		INetworkHelper* nh = obj->GetNetworkHelper();
		nh->PeerVisStatus[peerId] = 0;
	}
}

/*IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_Temp_Damage)
{
	obj_ServerPlayer* fromPlr = IsServerPlayer(fromObj);
	if(!fromPlr)
	{
		//r3dOutToLog("PKT_C2S_Temp_Damage: fromPlr is NULL\n");
		return;
	}
	
	if(fromPlr->peerId_ != peerId) 
	{
		// catch hackers here.
		LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Network, false, "TempDamagePeer",
			"peerID: %d, player: %d", 
			peerId, fromPlr->peerId_);
		return;
	}

	GameObject* target = GameWorld().GetNetworkObject(n.targetId);
	if(!target)
	{
		//r3dOutToLog("PKT_C2S_Temp_Damage: targetPlr is NULL\n");
		return;
	}

	obj_ServerPlayer* targetPlr = (obj_ServerPlayer*)target;
	if((targetPlr->profile_.ProfileData.isDevAccount && targetPlr->profile_.ProfileData.isGod) || (targetPlr->loadout_->GameFlags & wiCharDataFull::GAMEFLAG_NearPostBox) || (targetPlr->loadout_->GameFlags & wiCharDataFull::GAMEFLAG_SpawnProtection))
	{
		return;
	}

	const WeaponConfig* wc = g_pWeaponArmory->getWeaponConfig(101310);

	// check distance
	float dist = (n.explosion_pos-target->GetPosition()).Length();
	if(dist > wc->m_AmmoArea)
	{    
		//r3dOutToLog("PKT_C2S_Temp_Damage: dist is more than AmmoArea\n");
		return;
	}
	if ( n.damagePercentage > 100 || n.damagePercentage < 0 ) {
		
		r3dOutToLog("PKT_C2S_Temp_Damage: Damagepercentage was %d, which is incorrect, potentially a hack, disgarded.\n", n.damagePercentage);
		return;
	}

	float damage = wc->m_AmmoDamage*(1.0f-(dist/wc->m_AmmoArea));
	damage *= n.damagePercentage / 100.0f; // damage through wall

	r3dOutToLog("temp_damage from %s to %s, damage=%.2f\n", fromObj->Name.c_str(), target->Name.c_str(), damage); CLOG_INDENT;
	ApplyDamage(fromObj, target, n.explosion_pos, damage, true, wc->category);
}*/

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_Temp_Damage)
 {
   obj_ServerPlayer* fromPlr = IsServerPlayer(fromObj);
   if(!fromPlr)
   {
     r3dOutToLog("PKT_C2S_Temp_Damage: fromPlr is NULL\n");
     return;
   }
   
   if(fromPlr->peerId_ != peerId) 
   {
     // catch hackers here.
     LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Network, false, "TempDamagePeer",
       "peerID: %d, player: %d", 
       peerId, fromPlr->peerId_);
     return;
   }
 
   GameObject* target = GameWorld().GetNetworkObject(n.targetId);
   if(!target)
   {
     r3dOutToLog("PKT_C2S_Temp_Damage: targetPlr is NULL\n");
     return;
   }
 
   /*if(n.wpnIdx <0 || n.wpnIdx >= NUM_WEAPONS_ON_PLAYER)
   {
     r3dOutToLog("PKT_C2S_Temp_Damage: wpnIdx is out of range\n");
     return;
   }
   if(fromPlr->m_WeaponArray[n.wpnIdx]==NULL)
   {
     r3dOutToLog("PKT_C2S_Temp_Damage: m_WeaponArray[n.wpnIdx] is NULL\n");
     return;
   }*/
 
   //const WeaponConfig* wc = fromPlr->m_WeaponArray[n.wpnIdx]->getConfig();
   const WeaponConfig* wc = g_pWeaponArmory->getWeaponConfig(101310);
 
   // check distance
   float dist = (n.explosion_pos-target->GetPosition()).Length();
   if(dist > wc->m_AmmoArea)
   {    
     r3dOutToLog("PKT_C2S_Temp_Damage: dist is more than AmmoArea\n");
     return;
   }
   if ( n.damagePercentage > 300 || n.damagePercentage < 0 ) {
     
     r3dOutToLog("PKT_C2S_Temp_Damage: Damagepercentage was %d, which is incorrect, potentially a hack, disgarded.\n", n.damagePercentage);
     return;
   }
 
   float damage = wc->m_AmmoDamage*(1.0f-(dist/wc->m_AmmoArea));
   damage *= n.damagePercentage / 100.0f; // damage through wall
 
   r3dOutToLog("temp_damage from %s to %s, damage=%.2f\n", fromObj->Name.c_str(), target->Name.c_str(), damage); CLOG_INDENT;
   ApplyDamage(fromObj, target, n.explosion_pos, damage, true, wc->category);
}

int ServerGameLogic::ProcessChatCommand(obj_ServerPlayer* plr, const char* cmd)
{
	if(plr->profile_.ProfileData.isDevAccount)
    {
        if(strncmp(cmd, "/god", 4) == 0)
        {
            plr->profile_.ProfileData.isGod = !plr->profile_.ProfileData.isGod;

            r3dOutToLog("Player %s has %s god mode", plr->userName, (plr->profile_.ProfileData.isGod ? "enabled" : "disabled"));

            char message[64] = {0};
            sprintf(message, "God Mode %s", (plr->profile_.ProfileData.isGod ? "enabled" : "disabled"));
            PKT_C2C_ChatMessage_s n2;
            n2.userFlag = 0;
            n2.msgChannel = 0;
            r3dscpy(n2.msg, message);
            r3dscpy(n2.gamertag, "<system>");
            p2pSendToPeer(plr->peerId_, plr, &n2, sizeof(n2));

            return 0;
        }
    }
	r3dOutToLog("cmd: %s admin:%d\n", cmd, plr->profile_.ProfileData.isDevAccount);
	if(strncmp(cmd, "/tp", 3) == 0 && plr->profile_.ProfileData.isDevAccount)
		return Cmd_Teleport(plr, cmd);
  
	if(strncmp(cmd, "/goto",  5) == 0 && plr->profile_.ProfileData.isDevAccount)
        return Cmd_TPMTP(plr, cmd);

	if(strncmp(cmd, "/tome", 5) == 0 && plr->profile_.ProfileData.isDevAccount)
        return Cmd_TPPTM(plr, cmd);
    
	if(strncmp(cmd, "/kill", 5) == 0 && plr->profile_.ProfileData.isDevAccount)
        return Cmd_Kill(plr, cmd);
    
	if(strncmp(cmd, "/kick", 5) == 0 && plr->profile_.ProfileData.isDevAccount)
        return Cmd_Kick(plr, cmd);

	if(strncmp(cmd, "/gm", 3) == 0 && plr->profile_.ProfileData.isDevAccount)
        return Cmd_GodMode(plr, cmd);

	if(strncmp(cmd, "/gitem", 3) == 0 && plr->profile_.ProfileData.isDevAccount)
		return Cmd_GiveItem(plr, cmd);
		
	if(strncmp(cmd, "/sv", 3) == 0 && plr->profile_.ProfileData.isDevAccount)
		return Cmd_SetVitals(plr, cmd);
	
	if(strncmp(cmd, "/report", 7) == 0)
	{
        r3dAddReport("%s: %s \n", plr->userName, cmd);
        return 2;
	}
	
	return 1;	
}


int ServerGameLogic::Cmd_Kick(obj_ServerPlayer* plr, const char* cmd){
 
 char buf[128];
 char user[128];

 sscanf(cmd, "%s %s", buf, user);

 for(int i=0; i<MAX_PEERS_COUNT; ++i)
 {
  peerInfo_s& pr = GetPeer(i);
  if(pr.status_ == PEER_PLAYING && pr.player)
  {   
   if(strcmp(pr.player->userName, user) == 0) 
   { 
    r3dOutToLog("Player %s was kicked by Admin", pr.player->userName);
    DisconnectPeer(i, false, "Kicked from the game by admin: %s", pr.player->userName);
    return 0;
    break; 
   }
  }
 }
 r3dOutToLog("Player %s not Found!", user);
 return 2;
}
int ServerGameLogic::Cmd_Kill(obj_ServerPlayer* plr, const char* cmd){
    char buf[128];
    char user[128];

    sscanf(cmd, "%s %s", buf, user);

    for(int i=0; i<MAX_PEERS_COUNT; ++i)
    {
        peerInfo_s& pr = GetPeer(i);
        if(pr.status_ == PEER_PLAYING && pr.player)
        {            
            if(strcmp(pr.player->userName, user) == 0) 
            {    
                r3dOutToLog("Player %s killed by Admin", pr.player->userName);
                pr.player->DoDeath();
                return 0;
                break;    
            }
        }
    }
    r3dOutToLog("Player %s not Found!", user);
    return 2;
}

int ServerGameLogic::Cmd_GodMode(obj_ServerPlayer* plr, const char* cmd){
    char buf[128];
    int onoff;


    sscanf(cmd, "%s %d", buf, &onoff);


    plr->GodMode = onoff;
    r3dOutToLog("Setting godmode for %s to %d!", plr->userName, onoff);
    return 0;
}
int ServerGameLogic::Cmd_Teleport(obj_ServerPlayer* plr, const char* cmd)
{
	char buf[128];
	float x, z;
	
	if(3 != sscanf(cmd, "%s %f %f", buf, &x, &z))
		return 2;

	// cast ray down and find where we should place mine. should be in front of character, facing away from him
	PxRaycastHit hit;
	PxSceneQueryFilterData filter(PxFilterData(COLLIDABLE_STATIC_MASK, 0, 0, 0), PxSceneQueryFilterFlag::eSTATIC);
	if(!g_pPhysicsWorld->raycastSingle(PxVec3(x, 1000.0f, z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
	{
		r3dOutToLog("unable to teleport - no collision\n");
		return 2;
	}
	
	r3dPoint3D pos = AdjustPositionToFloor(r3dPoint3D(x, 0, z));
		
	PKT_S2C_MoveTeleport_s n;
	n.teleport_pos = pos;
	p2pBroadcastToActive(plr, &n, sizeof(n));
	plr->SetLatePacketsBarrier("teleport");
	plr->TeleportPlayer(pos);
	r3dOutToLog("%s teleported to %f, %f, %f\n", plr->userName, pos.x, pos.y, pos.z);
	return 0;
}

int ServerGameLogic::Cmd_TPMTP(obj_ServerPlayer* plr, const char* cmd){

    char buf[128];
    char user[128];

    sscanf(cmd, "%s %s", buf, user);
        

    for(int i=0; i<MAX_PEERS_COUNT; ++i)
    {
        peerInfo_s& pr = GetPeer(i);
        if(pr.status_ == PEER_PLAYING && pr.player)
        {
            
            if(strcmp(pr.player->userName, user) == 0) // found our User
            {        
               
                r3dPoint3D posx = pr.player->GetPosition();
                
                PxRaycastHit hit;
                PxSceneQueryFilterData filter(PxFilterData(COLLIDABLE_STATIC_MASK, 0, 0, 0), PxSceneQueryFilterFlag::eSTATIC);
                if(!g_pPhysicsWorld->raycastSingle(PxVec3(posx.x, 1000.0f, posx.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT, hit, filter))
                {
                    r3dOutToLog("unable to teleport - no collision near %s\n", pr.player->userName);
                    return 2;
                }
    
                r3dPoint3D pos2 = AdjustPositionToFloor(r3dPoint3D(posx.x, 0, posx.z));
        
                PKT_S2C_MoveTeleport_s n;
                n.teleport_pos = pos2;
                p2pBroadcastToActive(plr, &n, sizeof(n));
                plr->SetLatePacketsBarrier("teleport");
                plr->TeleportPlayer(pos2);
                r3dOutToLog("%s teleported to Player: %s at Location %f, %f, %f\n", plr->userName, pr.player->userName, pos2.x, pos2.y, pos2.z);
                return 0;
                break;
            }
        }
        r3dOutToLog("Cannot fine: %s!\n", user);
    }

    r3dOutToLog("There was an Error while Searching for Players!\n");
    return 2;
}


int ServerGameLogic::Cmd_TPPTM(obj_ServerPlayer* plr, const char* cmd){
    
    char buf[128];
    char user[128];

    sscanf(cmd, "%s %s", buf, user);

    for(int i=0; i<MAX_PEERS_COUNT; ++i)
    {
        peerInfo_s& pr = GetPeer(i);
        if(pr.status_ == PEER_PLAYING && pr.player)
        {            
            if(strcmp(pr.player->userName, user) == 0) // found
            {                        
                r3dPoint3D posx = plr->GetPosition();
                
                PxRaycastHit hit;
                PxSceneQueryFilterData filter(PxFilterData(COLLIDABLE_STATIC_MASK, 0, 0, 0), PxSceneQueryFilterFlag::eSTATIC);
                if(!g_pPhysicsWorld->raycastSingle(PxVec3(posx.x,  1000.0f, posx.z), PxVec3(0, -1, 0), 2000.0f, PxSceneQueryFlag::eIMPACT,  hit, filter))
                {
                    r3dOutToLog("unable to teleport - no collision\n" );
                    return 2;
                }
    
                r3dPoint3D pos2 = AdjustPositionToFloor(r3dPoint3D(posx.x, 0, posx.z));
        
                PKT_S2C_MoveTeleport_s n;
                n.teleport_pos = pos2;
                p2pBroadcastToActive(pr.player, &n, sizeof(n));
                pr.player->SetLatePacketsBarrier("teleport");
                pr.player->TeleportPlayer(pos2);
                r3dOutToLog("%s teleported to %f, %f, %f\n", plr->userName, pos2.x, pos2.y, pos2.z);
                return 0;
                break;
            }
        }
    }

    r3dOutToLog("Player not Found!\n");
    return 2;
    
}

int ServerGameLogic::Cmd_GiveItem(obj_ServerPlayer* plr, const char* cmd)
{
    char buf[128];
    int itemid, amount;
    
    if(3 != sscanf(cmd, "%s %d %d", buf, &itemid, &amount))
        return 2;

    if(g_pWeaponArmory->getConfig(itemid) == NULL) {
        r3dOutToLog("Cmd_GiveItem: no item %d\n", itemid);
        return 3;
    }

    wiInventoryItem wi;
    wi.itemID   = itemid;
    wi.quantity = amount;    
    plr->BackpackAddItem(wi);
    
    return 0;
}

int ServerGameLogic::Cmd_SetVitals(obj_ServerPlayer* plr, const char* cmd)
{
	char buf[128];
	int v1, v2, v3, v4;
	
	if(5 != sscanf(cmd, "%s %d %d %d %d", buf, &v1, &v2, &v3, &v4))
		return 2;
		
	plr->loadout_->Health = (float)v1;
	plr->loadout_->Hunger = (float)v2;
	plr->loadout_->Thirst = (float)v3;
	plr->loadout_->Toxic  = (float)v4;
	return 0;
}

//ViruZ Group
obj_ServerPlayer* ServerGameLogic::FindPlayer(char* Name)
{
	//FixedString c(Name);
	//c.ToLower();
	obj_ServerPlayer* tplr;
	for(int i=0; i<ServerGameLogic::MAX_NUM_PLAYERS; i++)
	{
		tplr = gServerLogic.GetPlayer(i);
		if(!tplr) continue;		
		//FixedString pn( tplr->userName);
		//pn.ToLower();
		if(strstr(Name,tplr->userName)) return tplr;
	}
	return NULL;
}
obj_ServerPlayer* ServerGameLogic::FindPlayerCustom(int CustomerID)
{
	obj_ServerPlayer* tplr;
	for(int i=0; i<ServerGameLogic::MAX_NUM_PLAYERS; i++)
	{
		tplr = gServerLogic.GetPlayer(i);
		if(!tplr) continue;		
		int pn = tplr->profile_.CustomerID;
		if(CustomerID == pn) return tplr;
	}
	return NULL;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_S2C_SendGroupNoAccept)
{
	char find[128];
	r3dscpy(find, n.intogamertag);
	obj_ServerPlayer* tplr = FindPlayer(find);

	char find1[128];
	r3dscpy(find1, n.fromgamertag);
	obj_ServerPlayer* plr = FindPlayer(find1);

	if (tplr)
	{
		char message2[128] = {0};
		PKT_S2C_ReceivedGroupInvite_s n3;
		sprintf(message2, "%s Decline Your Group Invite",plr->loadout_->Gamertag);
		r3dscpy(n3.text,message2);
		//gServerLogic.p2pSendToPeer(peerId_, NULL, &n2, sizeof(n2));
		gServerLogic.p2pSendToPeer(tplr->peerId_, tplr, &n3, sizeof(n3));

		char message3[128] = {0};
		PKT_S2C_ReceivedGroupInvite_s n4;
		n4.peerId = tplr->peerId_;
		sprintf(message3, "Decline Group Invited");
		r3dscpy(n4.text,message3);
		//gServerLogic.p2pSendToPeer(peerId_, NULL, &n2, sizeof(n2));
		gServerLogic.p2pSendToPeer(plr->peerId_, plr, &n3, sizeof(n3));

		tplr->loadout_->isInvite = false;
		plr->loadout_->isInvite = false;
	}
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_S2C_SendGroupAccept)
{
	char find[128];
	char find1[128];
	r3dscpy(find, n.intogamertag);
	obj_ServerPlayer* tplr = FindPlayer(find);
	r3dscpy(find1, n.fromgamertag);
	obj_ServerPlayer* plr = FindPlayer(find1);
	if (!plr || !tplr || tplr == plr)
		return;

	if (!tplr->loadout_->isInvite) return; // tplr must isInvite if no Packet Invaid return it.

	if (tplr->loadout_->GroupID == 0 && plr->loadout_->GroupID == 0)
	{
		float value;
		value = u_GetRandom(10, 9999999);
		tplr->loadout_->GroupID = (int)value;
		plr->loadout_->GroupID = tplr->loadout_->GroupID;
	}
	else if (tplr->loadout_->GroupID != 0) // Have Groups
	{
		plr->loadout_->GroupID = tplr->loadout_->GroupID;
	}
	else if (plr->loadout_->GroupID != 0) // Accpet player have groups
	{
		tplr->loadout_->GroupID = plr->loadout_->GroupID;
	}

	char chatmessage[128] = {0};
	PKT_C2C_ChatMessage_s n1;
	sprintf(chatmessage, "Player %s join groups",plr->loadout_->Gamertag);
	r3dscpy(n1.gamertag, "<System>");
	r3dscpy(n1.msg, chatmessage);
	n1.msgChannel = 3;
	n1.userFlag = 2;

	for(int i=0; i<MAX_PEERS_COUNT; i++) {
		if(peers_[i].status_ >= PEER_PLAYING && i != peerId && peers_[i].player) {
			if(plr->loadout_->GroupID == peers_[i].player->loadout_->GroupID)
				net_->SendToPeer(&n1, sizeof(n1), i, true);
		}
	}
	
	/*PKT_S2C_SetPlayerGroupID_s n1;
	n1.GroupID = tplr->loadout_->GroupID;*/
	//p2pBroadcastToAll(NULL, &n1, sizeof(n1), true);
	/*for(int i=0; i<MAX_PEERS_COUNT; i++) {
	if(peers_[i].status_ >= PEER_PLAYING && i != peerId && peers_[i].player) {
	if(tplr->loadout_->GroupID == peers_[i].player->loadout_->GroupID)
	//p2pBroadcastToActive(tplr, &n1, sizeof(n1), true);
	}
	}*/

	/*CJobGroupAdd* job = new CJobGroupAdd();
	job->CustomerID = tplr->profile_.CustomerID;
	job->CharID = tplr->loadout_->LoadoutID;
	g_AsyncApiMgr->AddJob(job);
	tplr->profile_.GetProfile(tplr->loadout_->LoadoutID);
	plr->loadout_->GroupID = tplr->loadout_->GroupID;/*
	}
	/*	char find[128];
	r3dscpy(find, n.intogamertag);
	obj_ServerPlayer* tplr = FindPlayer(find);

	char find1[128];
	r3dscpy(find1, n.fromgamertag);
	obj_ServerPlayer* plr = FindPlayer(find1);

	if (tplr)
	{

	PKT_S2C_SendGroupData_s n1;
	n1.peerid = plr->peerId_;
	n1.ShowCustomerID = plr->profile_.CustomerID;
	n1.MeCustomerID = tplr->profile_.CustomerID;
	r3dscpy(n1.gamertag,tplr->loadout_->Gamertag);
	n1.legend = true;
	gServerLogic.p2pSendToPeer(tplr->peerId_, tplr, &n1, sizeof(n1));

	PKT_S2C_SendGroupData_s n2;
	n2.peerid = plr->peerId_;
	n2.ShowCustomerID = plr->profile_.CustomerID;
	n2.MeCustomerID = tplr->profile_.CustomerID;
	r3dscpy(n2.gamertag,plr->loadout_->Gamertag);
	n2.legend = false;
	gServerLogic.p2pSendToPeer(tplr->peerId_, tplr, &n2, sizeof(n2));

	PKT_S2C_SendGroupData_s n3;
	n3.peerid = tplr->peerId_;
	n3.ShowCustomerID = tplr->profile_.CustomerID;
	n3.MeCustomerID = plr->profile_.CustomerID;
	r3dscpy(n3.gamertag,tplr->loadout_->Gamertag);
	n3.legend = true;
	gServerLogic.p2pSendToPeer(plr->peerId_, plr, &n3, sizeof(n3));

	PKT_S2C_SendGroupData_s n4;
	n4.peerid = tplr->peerId_;
	n4.ShowCustomerID = tplr->profile_.CustomerID;
	n4.MeCustomerID = plr->profile_.CustomerID;
	r3dscpy(n4.gamertag,plr->loadout_->Gamertag);
	n4.legend = false;
	gServerLogic.p2pSendToPeer(plr->peerId_, plr, &n4, sizeof(n4));

	plr->loadout_->isInvite = true;
	tplr->loadout_->isInvite = true;

	r3dscpy(tplr->loadout_->groupgamertag, n.fromgamertag);
	r3dscpy(plr->loadout_->groupgamertag, n.intogamertag);
	//tplr->loadout_->CustomerID = tplr->profile_.CustomerID;
	//plr->loadout_->CustomerID = plr->profile_.CustomerID;
	//tplr->loadout_->isInvite = false;
	*/
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_S2C_SendGroupInvite)
{
	char find[128];
	r3dscpy(find, n.intogamertag);
	obj_ServerPlayer* tplr = FindPlayer(find);

	char find1[128];
	r3dscpy(find1, n.fromgamertag);
	obj_ServerPlayer* plr = FindPlayer(find1);

	if (!tplr || !plr)
	{
		return;
	}
	if (plr == tplr)
	{
		return;
	}
	const float curTime = r3dGetTime();
	const float CHAT_DELAY_BETWEEN_MSG = 1.0f;	// expected delay between message
	const int   CHAT_NUMBER_TO_SPAM    = 5;		// number of messages below delay time to be considered spam
	float diff = curTime - plr->lastChatTime_;

	if (plr)
	{
		if(diff > CHAT_DELAY_BETWEEN_MSG) 
		{
			plr->numChatMessages_ = 0;
			plr->lastChatTime_    = curTime;
		}
		/*else // By Syntax - Precisa do pacote de mensagens "CheatMsg" do Server antigo
		{
			plr->numChatMessages_++;
			if(plr->numChatMessages_ >= CHAT_NUMBER_TO_SPAM)
			{
				char message[128] = {0};
				PKT_S2C_CheatMsg_s n2;
				sprintf(message, "Kicked from server : invalid PKT_C2C_SendGroupinvite - spam");
				r3dscpy(n2.cheatreason,message);
				//gServerLogic.p2pSendToPeer(peerId_, NULL, &n2, sizeof(n2));
				gServerLogic.p2pSendRawToPeer(plr->peerId_, &n2, sizeof(n2));
				DisconnectPeer(plr->peerId_, true, "invalid PKT_C2C_SendGroupinvite - spam");
				return;
			}
		}*/
	}


	if(tplr && plr)
	{
		if (!tplr->loadout_->isInvite)
		{
			char message1[128] = {0};
			char message2[128] = {0};
			PKT_S2C_ReceivedGroupInvite_s n2;
			n2.status = 2;
			n2.peerId = plr->peerId_;
			sprintf(message1, "Received group from %s",n.fromgamertag);
			r3dscpy(n2.text,message1);
			//gServerLogic.p2pSendToPeer(peerId_, NULL, &n2, sizeof(n2));
			gServerLogic.p2pSendToPeer(tplr->peerId_, tplr, &n2, sizeof(n2));

			PKT_S2C_ReceivedGroupInvite_s n3;
			//n3.status = 2;
			sprintf(message2, "Group invite send successfully",n.fromgamertag);
			r3dscpy(n3.text,message2);
			//n3.peerId = plr->peerId_;//99999;
			//gServerLogic.p2pSendToPeer(peerId_, NULL, &n2, sizeof(n2));
			gServerLogic.p2pSendToPeer(plr->peerId_, plr, &n3, sizeof(n3));

			tplr->loadout_->isInvite = true;
			plr->loadout_->isInvite = true;
			
		}
		else
		{
			char message2[128] = {0};
			PKT_S2C_ReceivedGroupInvite_s n3;
			sprintf(message2, "Allready Invited");
			r3dscpy(n3.text,message2);
			n3.peerId = plr->peerId_;//99999;*analisar*
			gServerLogic.p2pSendToPeer(plr->peerId_, plr, &n3, sizeof(n3));
			
		}
	}
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_S2C_ReceivedGroupInvite)
{
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_S2C_SendGroupData)
{
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_S2C_GetCustomerID)
{
	char find[128];
	r3dscpy(find, n.gamertag);
	obj_ServerPlayer* tplr = FindPlayer(find);

	char find1[128];
	r3dscpy(find1, n.fromgamertag);
	obj_ServerPlayer* plr = FindPlayer(find1);

	if(tplr)
	{
		PKT_S2C_GetCustomerID_s n1;

		n1.CustomerID = tplr->profile_.CustomerID;
		gServerLogic.p2pSendToPeer(plr->peerId_, plr, &n1, sizeof(n1));
	}
}
//ViruZ Group//

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2C_ChatMessage)
{
	if(!IsNullTerminated(n.gamertag, sizeof(n.gamertag))) {
		DisconnectPeer(peerId, true, "invalid PKT_C2C_ChatMessage #1");
		return;
	}
	if(!IsNullTerminated(n.msg, sizeof(n.msg))) {
		DisconnectPeer(peerId, true, "invalid PKT_C2C_ChatMessage #1");
		return;
	}
	if(n.userFlag != 0) {
		DisconnectPeer(peerId, true, "invalid PKT_C2C_ChatMessage #1 - flags");
		return;
	}

	// get player from peer, not from fromObj - more secure, no need to check for peer faking (player peer != p2p peer)
	obj_ServerPlayer* fromPlr = GetPeer(peerId).player;
	if(!fromPlr) {
		return;
	}

	// overwrite gamertag in packet, as hacker can send any crap he wants and post messages as someone else
	{
		PKT_C2C_ChatMessage_s* n_non_const = const_cast<PKT_C2C_ChatMessage_s*>(&n);
		r3dscpy(n_non_const->gamertag, fromPlr->userName);
	}

	if(fromPlr->profile_.ProfileData.AccountType == 0)
	{
		*const_cast<BYTE*>(&n.userFlag) |= 1;
	}
	if(fromPlr->profile_.ProfileData.isDevAccount)
	{
		*const_cast<BYTE*>(&n.userFlag) |= 2;
	}
	if(fromPlr->profile_.ProfileData.AccountType == 5) // Premium
	{
		*const_cast<BYTE*>(&n.userFlag) |= 4;
	}

	const float curTime = r3dGetTime();
	
	if(n.msg[0] == '/')
	{
		int res = ProcessChatCommand(fromPlr, n.msg);

		if( res == 0)
		{
			PKT_C2C_ChatMessage_s n2;
			n2.userFlag = 0;
			n2.msgChannel = 1;
			r3dscpy(n2.msg, "command executed");
			r3dscpy(n2.gamertag, "<system>");
			p2pSendToPeer(peerId, fromPlr, &n2, sizeof(n2));
		}
		else if(res == 2)    
        {
            PKT_C2C_ChatMessage_s n2;
            n2.userFlag = 0;
            n2.msgChannel = 3;
            sprintf(n2.msg, "Player Reported");
            r3dscpy(n2.gamertag, "<System>");
            p2pSendToPeer(peerId, fromPlr, &n2, sizeof(n2));
        }
		else
		{
			PKT_C2C_ChatMessage_s n2;
			n2.userFlag = 0;
			n2.msgChannel = 1;
			sprintf(n2.msg, "no such command, %d", res);
			r3dscpy(n2.gamertag, "<system>");
			p2pSendToPeer(peerId, fromPlr, &n2, sizeof(n2));
		}
		return;
	}
	
	// check for chat spamming
	const float CHAT_DELAY_BETWEEN_MSG = 1.0f;	// expected delay between message
	const int   CHAT_NUMBER_TO_SPAM    = 4;		// number of messages below delay time to be considered spam
	float diff = curTime - fromPlr->lastChatTime_;

	if(diff > CHAT_DELAY_BETWEEN_MSG) 
	{
		fromPlr->numChatMessages_ = 0;
		fromPlr->lastChatTime_    = curTime;
	}
	else 
	{
		fromPlr->numChatMessages_++;
		if(fromPlr->numChatMessages_ >= CHAT_NUMBER_TO_SPAM)
		{
			DisconnectPeer(peerId, true, "invalid PKT_C2C_ChatMessage #3 - spam");
			return;
		}
	}
	
	// note
	//   do not use p2p function here as they're visibility based now

	switch( n.msgChannel ) 
	{
		case 3: //ViruZ Group
			{
			if(fromPlr->loadout_->GroupID != 0)
			{
				for(int i=0; i<MAX_PEERS_COUNT; i++) {
					if(peers_[i].status_ >= PEER_PLAYING && i != peerId && peers_[i].player) {
						if(fromPlr->loadout_->GroupID == peers_[i].player->loadout_->GroupID)
							net_->SendToPeer(&n, sizeof(n), i, true);
					}
				}
			}
		}
        break;

		case 2: // clan
		{
			if(fromPlr->loadout_->ClanID != 0)
			{
				for(int i=0; i<MAX_PEERS_COUNT; i++) {
					if(peers_[i].status_ >= PEER_PLAYING && i != peerId && peers_[i].player) {
						if(fromPlr->loadout_->ClanID == peers_[i].player->loadout_->ClanID)
							net_->SendToPeer(&n, sizeof(n), i, true);
					}
				}
			}
		}
		break;

		case 1: // global
		{
			for(int i=0; i<MAX_PEERS_COUNT; i++) {
				if(peers_[i].status_ >= PEER_PLAYING && i != peerId && peers_[i].player) {
					net_->SendToPeer(&n, sizeof(n), i, true);
				}
			}
		}
		break;
		
		case 0:  // proximity
			for(int i=0; i<MAX_PEERS_COUNT; i++) {
				if(peers_[i].status_ >= PEER_PLAYING && i != peerId && peers_[i].player) {
					if((peers_[i].player->GetPosition() - fromPlr->GetPosition()).Length() < 200.0f)
						net_->SendToPeer(&n, sizeof(n), i, true);
				}
			}
			break;
		default:
		{
			DisconnectPeer(peerId, true, "invalid PKT_C2C_ChatMessage #4 - wrong msgChannel");
			return;
		}
		break;
		
	}
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_DataUpdateReq)
{
	r3dOutToLog("got PKT_C2S_DataUpdateReq\n");
	
	// relay that event to master server.
	//gMasterServerLogic.RequestDataUpdate();
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_Admin_PlayerKick)
{
	peerInfo_s& peer = GetPeer(peerId);

	// check if received from legitimate admin account
	if(!peer.player || !peer.temp_profile.ProfileData.isDevAccount)
		return;

	// go through all peers and find a player with netID
	for(int i=0; i<MAX_PEERS_COUNT; ++i)
	{
		peerInfo_s& pr = GetPeer(i);
		if(pr.status_ == PEER_PLAYING && pr.player)
		{
			if(pr.player->GetNetworkID() == n.netID) // found
			{
				DisconnectPeer(i, false, "Kicked from the game by admin: %s", pr.player->userName);
				break;
			}
		}
	}
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_Admin_GiveItem)
{
	peerInfo_s& peer = GetPeer(peerId);

	// check if received from legitimate admin account
	if(!peer.player || !peer.temp_profile.ProfileData.isDevAccount)
		return;
		
	if(g_pWeaponArmory->getConfig(n.ItemID) == NULL) {
		r3dOutToLog("PKT_C2S_Admin_GiveItem: no item %d\n", n.ItemID);
		return;
	}

	wiInventoryItem wi;
	wi.itemID   = n.ItemID;
	wi.quantity = 1;	
	peer.player->BackpackAddItem(wi);
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_SecurityRep)
{
	const float curTime = r3dGetTime();
	peerInfo_s& peer = GetPeer(peerId);
	if(peer.player==NULL) // cheat??
		return;

	if(peer.secRepGameTime < 0)
	{
		// first call.
		peer.secRepRecvTime = curTime;
		peer.secRepGameTime = n.gameTime;
		//r3dOutToLog("peer%02d, CustomerID:%d SecRep started\n");
		return;
	}
	
	float delta1 = n.gameTime - peer.secRepGameTime;
	float delta2 = curTime    - peer.secRepRecvTime;

	//@ ignore small values for now, until we resolve how that can happens without cheating.
	if(delta2 > ((float)PKT_C2S_SecurityRep_s::REPORT_PERIOD - 0.3f) && delta2 < PKT_C2S_SecurityRep_s::REPORT_PERIOD)
		delta2 = PKT_C2S_SecurityRep_s::REPORT_PERIOD;

	// account for late packets
	peer.secRepRecvAccum -= (delta2 - PKT_C2S_SecurityRep_s::REPORT_PERIOD);

	float k = delta1 - delta2;
	bool isLag = (k > 1.0f || k < -1.0f);
	
	/*
	r3dOutToLog("peer%02d, CID:%d SecRep: %f %f %f %f %s\n", 
		peerId, peer.CustomerID, delta1, delta2, k, peer.secRepRecvAccum,
		isLag ? "net_lag" : "");*/

	// check for client timer
	if(fabs(delta1 - PKT_C2S_SecurityRep_s::REPORT_PERIOD) > 1.0f)
	{
		LogInfo(peerId,	"client_lag?", "%f, %f, %f", delta1, delta2, peer.secRepRecvAccum);
	}

	// check if client was sending packets faster that he should, 20% limit
	if(peer.secRepRecvAccum > ((float)PKT_C2S_SecurityRep_s::REPORT_PERIOD * 0.2f))
	{
		LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_SpeedHack, true,	"speedhack",
			"%f, %f, %f", delta1, delta2, peer.secRepRecvAccum
			);

		peer.secRepRecvAccum = 0;
	}

	// add check for d3d cheats
	if(n.detectedWireframeCheat)
	{
		LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Wireframe, false, "wireframe cheat");
	}
	
	if((GPP_Data.GetCrc32() ^ GPP_Seed) != n.GPP_Crc32)
	{
		LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_GPP, true, "GPP cheat");
	}

	peer.secRepRecvTime = curTime;
	peer.secRepGameTime = n.gameTime;
	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_UseNetObject)
{
	//LogInfo(peerId, "PKT_C2S_UseNetObject", "%d", n.spawnID); CLOG_INDENT;

	// get player from peer, not from fromObj - more secure, no need to check for peer faking (player peer != p2p peer)
	obj_ServerPlayer* fromPlr = GetPeer(peerId).player;
	if(!fromPlr) {
		return;
	}

	if(fromPlr->loadout_->Alive == 0) {
		// he might be dead on server, but still didn't know that on client
		return;
	}
		
	GameObject* base = GameWorld().GetNetworkObject(n.spawnID);
	if(!base) {
		// this is valid situation, as network item might be already despawned
		return;
	}

	// multiple players can try to activate it
	if(!base->isActive())
		return;

	// validate range (without Y)
	{
		r3dPoint3D bpos = base->GetPosition(); bpos.y = 0.0f;
		r3dPoint3D ppos = fromPlr->GetPosition(); ppos.y = 0.0f;
		float dist = (bpos - ppos).Length();
		if(dist > 5.0f)
		{
			gServerLogic.LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Protocol, true, "UseNetObject",
				"dist %f", dist);
			return;
		}
	}

	if(base->Class->Name == "obj_SpawnedItem")
	{
		obj_SpawnedItem* obj = (obj_SpawnedItem*)base;
		if(fromPlr->BackpackAddItem(obj->m_Item))
			obj->setActiveFlag(0);
	}
	else if(base->Class->Name == "obj_DroppedItem")
	{
		obj_DroppedItem* obj = (obj_DroppedItem*)base;
		if(fromPlr->BackpackAddItem(obj->m_Item))
			obj->setActiveFlag(0);
	}
	else if(base->Class->Name == "obj_Note")
	{
		obj_Note* obj = (obj_Note*)base;
		obj->NetSendNoteData(peerId);
	}
	
	else if(base->Class->Name == "obj_Grave")//Gravestone
    {
      obj_Grave* obj = (obj_Grave*)base;
      obj->NetSendGraveData(peerId);
    }

	else
	{
		LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Protocol, false, "UesNetObject",
			"obj %s", base->Class->Name.c_str());
	}

	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_CreateNote)
{
	// get player from peer, not from fromObj - more secure, no need to check for peer faking (player peer != p2p peer)
	obj_ServerPlayer* fromPlr = GetPeer(peerId).player;
	if(!fromPlr) {
		return;
	}

	if(!IsNullTerminated(n.TextFrom, sizeof(n.TextFrom)) || !IsNullTerminated(n.TextSubj, sizeof(n.TextSubj))) {
		gServerLogic.LogCheat(peerId, PKT_S2C_CheatWarning_s::CHEAT_Protocol, true, "PKT_C2S_CreateNote",
			"no null in text");
		return;
	}
	
	// relay logic to player
	fromPlr->UseItem_CreateNote(n);
	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_TEST_SpawnDummyReq)
{
	r3dOutToLog("!!!!!!! NOPE: dummies not implemented\n");
	r3d_assert(fromObj);
	r3d_assert(IsServerPlayer(fromObj));

	return;
}

IMPL_PACKET_FUNC(ServerGameLogic, PKT_C2S_DBG_LogMessage)
{
	// get player from peer, not from fromObj - more secure, no need to check for peer faking (player peer != p2p peer)
	obj_ServerPlayer* fromPlr = GetPeer(peerId).player;
	if(!fromPlr) {
		return;
	}

	// log that packet with temp cheat code
	LogCheat(fromPlr->peerId_, 98, false, "clientlog",
		"%s", 
		n.msg
		);
	return;
}

void ServerGameLogic::OnPKT_C2S_ScreenshotData(DWORD peerId, const int size, const char* data)
{
	char	fname[MAX_PATH];

	const peerInfo_s& peer = GetPeer(peerId);
	if(peer.player == NULL) {
		return;
	} else {
		sprintf(fname, "logss\\JPG_%d_%d_%d_%x.jpg", ginfo_.gameServerId, peer.player->profile_.CustomerID, peer.player->loadout_->LoadoutID, GetTickCount());
	}

	r3dOutToLog("peer%02d received screenshot, fname:%s", peerId, fname);

	FILE* f = fopen(fname, "wb");
	if(f == NULL) {
		LogInfo(peerId, "SaveScreenshot", "unable to save fname:%s", fname);
		return;
	}
	fwrite(data, 1, size, f);
	fclose(f);

	return;
}


int ServerGameLogic::ProcessWorldEvent(GameObject* fromObj, DWORD eventId, DWORD peerId, const void* packetData, int packetSize)
{
	// do version check and game join request
	peerInfo_s& peer = GetPeer(peerId);

	switch(peer.status_)
	{
		// check version in connected state
	case PEER_CONNECTED:
		switch(eventId)
		{
			DEFINE_PACKET_HANDLER(PKT_C2S_ValidateConnectingPeer);
		}
		DisconnectPeer(peerId, true, "bad packet ID %d in connected state", eventId);
		return TRUE;

		// process join request in validated state
	case PEER_VALIDATED1:
		switch(eventId)
		{
			DEFINE_PACKET_HANDLER(PKT_C2S_JoinGameReq);
		}
		DisconnectPeer(peerId, true, "bad packet ID %d in validated1 state", eventId);
		return TRUE;

	case PEER_LOADING:
		switch(eventId)
		{
			DEFINE_PACKET_HANDLER(PKT_C2S_StartGameReq);
		}
		DisconnectPeer(peerId, true, "bad packet ID %d in loading state", eventId);
		return TRUE;
	}

	r3d_assert(peer.status_ == PEER_PLAYING);

	// validation and relay client code
	switch(eventId) 
	{
		DEFINE_PACKET_HANDLER(PKT_C2S_Temp_Damage);
		//ViruZ Group
		DEFINE_PACKET_HANDLER(PKT_S2C_SendGroupInvite);
		DEFINE_PACKET_HANDLER(PKT_S2C_SendGroupAccept);
		DEFINE_PACKET_HANDLER(PKT_S2C_SendGroupNoAccept);
		DEFINE_PACKET_HANDLER(PKT_S2C_SendGroupData);
		DEFINE_PACKET_HANDLER(PKT_S2C_ReceivedGroupInvite);
		DEFINE_PACKET_HANDLER(PKT_S2C_GetCustomerID);
		//ViruZ Group//
		DEFINE_PACKET_HANDLER(PKT_C2C_ChatMessage);
		DEFINE_PACKET_HANDLER(PKT_C2S_DataUpdateReq);

		DEFINE_PACKET_HANDLER(PKT_C2S_SecurityRep);
		DEFINE_PACKET_HANDLER(PKT_C2S_Admin_PlayerKick);
		DEFINE_PACKET_HANDLER(PKT_C2S_Admin_GiveItem);
		DEFINE_PACKET_HANDLER(PKT_C2S_UseNetObject);
		DEFINE_PACKET_HANDLER(PKT_C2S_CreateNote);
		DEFINE_PACKET_HANDLER(PKT_C2S_TEST_SpawnDummyReq);
		
		DEFINE_PACKET_HANDLER(PKT_C2S_DBG_LogMessage);
		
		// special packet case with variable length
		case PKT_C2S_ScreenshotData:
		{
			const PKT_C2S_ScreenshotData_s& n = *(PKT_C2S_ScreenshotData_s*)packetData;
			if(packetSize < sizeof(n)) {
				LogInfo(peerId, "PKT_C2S_ScreenshotData", "packetSize %d < %d", packetSize, sizeof(n));
				return TRUE;
			}
			if(n.errorCode != 0)
			{
				LogInfo(peerId, "PKT_C2S_ScreenshotData", "screenshot grab failed: %d", n.errorCode);
				return TRUE;
			}
			
			if(packetSize != sizeof(n) + n.dataSize) {
				LogInfo(peerId, "PKT_C2S_ScreenshotData", "dataSize %d != %d+%d", packetSize, sizeof(n), n.dataSize);
				return TRUE;
			}
			
			OnPKT_C2S_ScreenshotData(peerId, n.dataSize, (char*)packetData + sizeof(n));
			return TRUE;
		}
	}

	return FALSE;
}

void ServerGameLogic::TrackWeaponUsage(uint32_t ItemID, int ShotsFired, int ShotsHits, int Kills)
{
	WeaponStats_s* ws = NULL;
	for(size_t i = 0, size = weaponStats_.size(); i < size; ++i)
	{
		if(weaponStats_[i].ItemID == ItemID)
		{
			ws = &weaponStats_[i];
			break;
		}
	}
	
	if(ws == NULL)
	{
		weaponStats_.push_back(WeaponStats_s());
		ws = &weaponStats_.back();
		ws->ItemID = ItemID;
	}
	
	r3d_assert(ws);
	ws->ShotsFired += ShotsFired;
	ws->ShotsHits  += ShotsHits;
	ws->Kills      += Kills;
	return;
}

void ServerGameLogic::Tick()
{
	r3d_assert(maxPlayers_ > 0);
	net_->Update();
	
	const float curTime = r3dGetTime();

	// shutdown notify logic
	if(gMasterServerLogic.shuttingDown_)
	{
	  // send note every 1 sec
	  static float lastSent = 999999;
	  if(fabs(lastSent - gMasterServerLogic.shutdownLeft_) > 1.0f)
	  {
	    lastSent = gMasterServerLogic.shutdownLeft_;
	    r3dOutToLog("sent shutdown note\n");
	  
	    PKT_S2C_ShutdownNote_s n;
	    n.reason   = 0;
	    n.timeLeft = gMasterServerLogic.shutdownLeft_;
	    p2pBroadcastToAll(NULL, &n, sizeof(n), true);
	  }

	  // close game when shutdown
	  if(gMasterServerLogic.shutdownLeft_ < 0)
		throw "shutting down....";
	}

	CheckClientsSecurity();
	
	g_AsyncApiMgr->Tick();

	if(gameFinished_)
		return;

	/*DISABLED, as it complicate things a bit.
	if(gMasterServerLogic.gotWeaponUpdate_)
	{
		gMasterServerLogic.gotWeaponUpdate_ = false;

		weaponDataUpdates_++;
		SendWeaponsInfoToPlayer(true, 0);
	}*/

	//@@@ kill all players
	if(GetAsyncKeyState(VK_F11) & 0x8000) 
	{
		r3dOutToLog("trying to kill all players\n");
		for(int i=0; i<maxPlayers_; i++) {
			obj_ServerPlayer* plr = GetPlayer(i);
			if(!plr || plr->loadout_->Alive == 0)
				continue;

			DoKillPlayer(plr, plr, storecat_INVALID, true);
		}
	}

	static float nextDebugLog_ = 0;
	if(curTime > nextDebugLog_) 
	{
		char apiStatus[128];
		g_AsyncApiMgr->GetStatus(apiStatus);
		
		nextDebugLog_ = curTime + 60.0f;
		r3dOutToLog("time: %.0f, plrs:%d/%d, net_lastFreeId: %d, objects: %d, async:%s\n", 
			r3dGetTime() - gameStartTime_, 
			curPlayers_, ginfo_.maxPlayers,
			net_lastFreeId,
			GameWorld().GetNumObjects(),
			apiStatus);
	}

	return;
}

void ServerGameLogic::DumpPacketStatistics()
{
  __int64 totsent = 0;
  __int64 totrecv = 0;
  
  for(int i=0; i<R3D_ARRAYSIZE(netRecvPktSize); i++) {
    totsent += netSentPktSize[i];
    totrecv += netRecvPktSize[i];
  }

  r3dOutToLog("Packet Statistics: out:%I64d in:%I64d, k:%f\n", totsent, totrecv, (float)totsent/(float)totrecv);
  CLOG_INDENT;
  
  for(int i=0; i<R3D_ARRAYSIZE(netRecvPktSize); i++) {
    if(netSentPktSize[i] == 0 && netRecvPktSize[i] == 0)
      continue;
      
    r3dOutToLog("%3d: out:%10I64d in:%10I64d out%%:%.1f%%\n", 
      i, 
      netSentPktSize[i],
      netRecvPktSize[i],
      (float)netSentPktSize[i] * 100.0f / float(totsent));
  }
  
}

__int64 ServerGameLogic::GetUtcGameTime()
{
	// "world time start" offset, so gametime at 1st sep 2012 will be in 2018 range
	struct tm toff = {0};
	toff.tm_year   = 2011-1900;
	toff.tm_mon    = 6;
	toff.tm_mday   = 1;
	toff.tm_isdst  = -1; // A value less than zero to have the C run-time library code compute whether standard time or daylight saving time is in effect.
	__int64 secs0 = _mkgmtime64(&toff);	// world start time
	__int64 secs1 = _time64(&secs1);	// current UTC time

	// reassemble time, with speedup factor
	return secs0 + (secs1 - secs0) * (__int64)GPP_Data.c_iGameTimeCompression;
}

void ServerGameLogic::SendWeaponsInfoToPlayer(DWORD peerId)
{
	//r3dOutToLog("sending weapon info to peer %d\n", peerId);

	const peerInfo_s& peer = GetPeer(peerId);

	g_pWeaponArmory->startItemSearch();
	while(g_pWeaponArmory->searchNextItem())
	{
		uint32_t itemID = g_pWeaponArmory->getCurrentSearchItemID();
		const WeaponConfig* weaponConfig = g_pWeaponArmory->getWeaponConfig(itemID);
		if(weaponConfig)
		{
			PKT_S2C_UpdateWeaponData_s n;
			n.itemId = weaponConfig->m_itemID;
			weaponConfig->copyParametersTo(n.wi);
			p2pSendRawToPeer(peerId, &n, sizeof(n), true);
		}

		/* no need to send gear configs for now - there is nothing interesting
		const GearConfig* gearConfig = g_pWeaponArmory->getGearConfig(itemID);
		if(gearConfig)
		{
			PKT_S2C_UpdateGearData_s n;
			n.itemId = gearConfig->m_itemID;
			gearConfig->copyParametersTo(n.gi);
			p2pSendRawToPeer(peerId, &n, sizeof(n), true);
		}
		*/
	}

	return;
}
