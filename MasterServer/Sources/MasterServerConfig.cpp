#include "r3dPCH.h"
#include "r3d.h"

#include "MasterServerConfig.h"

	CMasterServerConfig* gServerConfig = NULL;

static const char* configFile = "MasterServer.cfg";

CMasterServerConfig::CMasterServerConfig()
{
  const char* group      = "MasterServer";

  if(_access(configFile, 0) != 0) {
    r3dError("can't open config file %s\n", configFile);
  }

  masterPort_  = r3dReadCFG_I(configFile, group, "masterPort", SBNET_MASTER_PORT);
  clientPort_  = r3dReadCFG_I(configFile, group, "clientPort", GBNET_CLIENT_PORT);
  masterCCU_   = r3dReadCFG_I(configFile, group, "masterCCU",  3000);

  supervisorCoolDownSeconds_ = r3dReadCFG_F(configFile, group, "supervisorCoolDownSeconds",  15.0f);

  #define CHECK_I(xx) if(xx == 0)  r3dError("missing %s value in %s", #xx, configFile);
  #define CHECK_S(xx) if(xx == "") r3dError("missing %s value in %s", #xx, configFile);
  CHECK_I(masterPort_);
  CHECK_I(clientPort_);
  #undef CHECK_I
  #undef CHECK_S

  serverId_    = r3dReadCFG_I(configFile, group, "serverId", 0);
  if(serverId_ == 0)
  {
	MessageBox(NULL, "you must define serverId in MasterServer.cfg", "", MB_OK);
	r3dError("no serverId");
  }
  if(serverId_ > 255 || serverId_ < 1)
  {
	MessageBox(NULL, "bad serverId", "", MB_OK);
	r3dError("bad serverId");
  }
  
  LoadConfig();
  
  return;
}

void CMasterServerConfig::LoadConfig()
{
  r3dCloseCFG_Cur();
  
  numPermGames_ = 0;

  LoadPermGamesConfig();
  Temp_Load_WarZGames();
}

void CMasterServerConfig::Temp_Load_WarZGames()
{
  char group[128];
  sprintf(group, "WarZGames");

  int numGames   = r3dReadCFG_I(configFile, group, "numGames", 0);
  int maxPlayers = r3dReadCFG_I(configFile, group, "maxPlayers", 32);
  
  r3dOutToLog("WarZ %d games, %d players each\n", numGames, maxPlayers);
  
  for(int i=0; i<numGames; i++)
  {
    GBGameInfo ginfo;
    ginfo.mapId      = GBGameInfo::MAPID_WZ_Colorado;
    ginfo.maxPlayers = maxPlayers;

    sprintf(ginfo.name, "US Server %03d", i + 1);
    AddPermanentGame(10000 + i, ginfo, GBNET_REGION_US_West);

    sprintf(ginfo.name, "EU Server %03d", i + 1);
    AddPermanentGame(20000 + i, ginfo, GBNET_REGION_Europe);
  }
}

void CMasterServerConfig::LoadPermGamesConfig()
{
  numPermGames_ = 0;

//#ifdef _DEBUG
//  r3dOutToLog("Permanet games disabled in DEBUG");
//  return;
//#endif
  
  for(int i=0; i<250; i++)
  {
    char group[128];
    sprintf(group, "PermGame%d", i+1);

    char map[512] = "";
    char data[512] = "";
	char pwdchar[512];
    char name[512];
	//char pwd[512];
	int ispwd;
	char ispwd1[512];
	int ispre2;
	char ispre1[512];
	r3dscpy(ispwd1,  r3dReadCFG_S(configFile, group, "ispwd", ""));
	r3dscpy(ispre1,  r3dReadCFG_S(configFile, group, "ispre", ""));
    r3dscpy(map,  r3dReadCFG_S(configFile, group, "map", ""));
    r3dscpy(data, r3dReadCFG_S(configFile, group, "data", ""));
    r3dscpy(name, r3dReadCFG_S(configFile, group, "name", ""));
	r3dscpy(pwdchar, r3dReadCFG_S(configFile, group, "pwd", ""));
    if(name[0] == 0)
      sprintf(name, "PermGame%d", i+1);

    if(*map == 0)
      continue;
    
	sscanf(ispwd1, "%d", &ispwd);
	sscanf(ispre1, "%d", &ispre2);
	
	bool ispre = false;
		if (ispre2 == 1)
		{
		ispre = true;
		}

	//sscanf(ispwd1, "%d", &ispwd);
	bool ispass = false;
		if (ispwd == 1)
		{
		ispass = true;
		}

	bool isfarm = false;
		if (ispass && ispre)
		{
		ispass = false;
		ispre = false;
		isfarm = true;
		}
    
    ParsePermamentGame(i, name, map, data,pwdchar,ispass,ispre,isfarm);
  }

  return;  
}

static int StringToGBMapID(char* str)
{
  if(stricmp(str, "MAPID_WZ_Colorado") == 0)
    return GBGameInfo::MAPID_WZ_Colorado;
  if(stricmp(str, "MAPID_WZ_Cliffside") == 0)
    return GBGameInfo::MAPID_WZ_Cliffside;
  if(stricmp(str, "MAPID_WZ_Colorado_pve") == 0)
    return GBGameInfo::MAPID_WZ_Colorado_pve;
  if(stricmp(str, "MAPID_WZ_Atlanta") == 0)
    return GBGameInfo::MAPID_WZ_Atlanta;
  if(stricmp(str, "MAPID_WZ_ViruZ_pvp") == 0)
    return GBGameInfo::MAPID_WZ_ViruZ_pvp;
  if(stricmp(str, "MAPID_WZ_Valley") == 0)
    return GBGameInfo::MAPID_WZ_Valley;


  
  if(stricmp(str, "MAPID_Editor_Particles") == 0)
    return GBGameInfo::MAPID_Editor_Particles;
  if(stricmp(str, "MAPID_ServerTest") == 0)
    return GBGameInfo::MAPID_ServerTest;
    
  r3dError("bad GBMapID %s\n", str);
  return 0;
}

static EGBGameRegion StringToGBRegion(const char* str)
{
  if(stricmp(str, "GBNET_REGION_US_West") == 0)
    return GBNET_REGION_US_West;
  if(stricmp(str, "GBNET_REGION_US_East") == 0)
    return GBNET_REGION_US_East;
  if(stricmp(str, "GBNET_REGION_Europe") == 0)
    return GBNET_REGION_Europe;
  if(stricmp(str, "GBNET_REGION_Russia") == 0)
    return GBNET_REGION_Russia;
    
  r3dError("bad GBGameRegion %s\n", str);
  return GBNET_REGION_Unknown;
}

void CMasterServerConfig::ParsePermamentGame(int gameServerId, const char* name, const char* map, const char* data,const char* pwdchar,bool ispass,bool ispre,bool isfarm)
{
  char mapid[128];
  char maptype[128];
  char region[128];
  int minGames;
  int maxGames;
  if(5 != sscanf(map, "%s %s %s %d %d", mapid, maptype, region, &minGames, &maxGames)) {
    r3dError("bad map format: %s\n", map);
  }

  int maxPlayers;
  int minLevel = 0;
  int maxLevel = 0;
  if(3 != sscanf(data, "%d %d %d", &maxPlayers, &minLevel, &maxLevel)) {
    r3dError("bad data format: %s\n", data);
  }

  GBGameInfo ginfo;
  ginfo.mapId        = StringToGBMapID(mapid);
  ginfo.maxPlayers   = maxPlayers;
  ginfo.ispass = ispass;
  ginfo.ispre = ispre;
  ginfo.isfarm = isfarm;
  r3dscpy(ginfo.name, name);
  r3dscpy(ginfo.pwdchar, pwdchar);

  r3dOutToLog("permgame: ID:%d, %s, %s,%s\n", 
    gameServerId, name, mapid,pwdchar);
  
  EGBGameRegion eregion = StringToGBRegion(region);
  AddPermanentGame(gameServerId, ginfo, eregion);
}

void CMasterServerConfig::AddPermanentGame(int gameServerId, const GBGameInfo& ginfo, EGBGameRegion region)
{
  r3d_assert(numPermGames_ < R3D_ARRAYSIZE(permGames_));
  permGame_s& pg = permGames_[numPermGames_++];

  r3d_assert(gameServerId);
  pg.ginfo = ginfo;
  pg.ginfo.gameServerId = gameServerId;
  pg.ginfo.region       = region;
  
  return;
}
