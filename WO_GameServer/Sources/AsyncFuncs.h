#pragma once

#include "Backend/ServerUserProfile.h"

class obj_ServerPlayer;

class CAsyncApiJob
{
  public:
	char		desc[156];
  
	int		peerId;		// or -1 if this is peerless operation
	DWORD		CustomerID;
	DWORD		SessionID;
	DWORD		CharID;
	
	int		ResultCode;
	
  public:
	CAsyncApiJob();
	CAsyncApiJob(const obj_ServerPlayer* plr);
virtual	~CAsyncApiJob() { }
	
virtual	int		Exec() = NULL;
virtual	void		OnSuccess() = NULL;
};

class CAsyncApiWorker
{
  public:
	int		idx_;
	HANDLE		hThread;
	
	HANDLE		hStartEvt_;
	bool		needToExit_;

	CRITICAL_SECTION csJobs_;
	std::list<CAsyncApiJob*> jobs_;
	std::list<CAsyncApiJob*> finished_;
	CAsyncApiJob*	curJob_;

	int		WorkerThread();
	CAsyncApiJob*	 GetNextJob();
	void		 ProcessJob(CAsyncApiJob* job);
  
  public:
	CAsyncApiWorker();
	~CAsyncApiWorker();
};

class CAsyncApiMgr
{
  public:
	enum { NUM_WORKER_THREADS = 4, };
	CAsyncApiWorker	workers_[NUM_WORKER_THREADS];

	void		AddJob(CAsyncApiJob* job);
	void		StartWorkers();
	
  public:
	CAsyncApiMgr();
	~CAsyncApiMgr();
	
	void		Tick();
	void		TerminateAll();
	void		GetStatus(char* text);
};
extern CAsyncApiMgr*	g_AsyncApiMgr;


//
//
// specific API calls
//
//

class CJobProcessUserJoin : public CAsyncApiJob
{
	CServerUserProfile prof;
	int		GameJoinResult;

  public:
	CJobProcessUserJoin(int in_peerId);

	int		Exec();
	void		OnSuccess();
};
//ViruZ Group
class CJobGroupAdd : public CAsyncApiJob
{
public:
	int		Exec();
	void		OnSuccess() {}
};
//ViruZ Group//

class CJobUserLeftGame : public CAsyncApiJob
{
  public:
	int		TimePlayed;
	int		GameMapId;
	DWORD		GameServerId;
  
  public:
	CJobUserLeftGame(const obj_ServerPlayer* plr) : CAsyncApiJob(plr)
	{
		sprintf(desc, "CJobUserLeftGame[%d] %p", CustomerID, this);
		GameMapId    = 0;
		GameServerId = 0;
		TimePlayed   = 0;
	}

	int		Exec();
	void		OnSuccess() {}
};

class CJobUpdateChar : public CAsyncApiJob
{
  public:
	wiCharDataFull	CharData;
	wiCharDataFull	OldData;
	int		GameDollars;

	const static int DISCONNECT_WAIT_TIME = 10; // time player must wait before actual disconnect (to prevent exiting from battlefield while fighting)
	bool		Disconnect;	// disconnect after update
  
  public:
	CJobUpdateChar(const obj_ServerPlayer* plr) : CAsyncApiJob(plr)
	{
		sprintf(desc, "CJobUpdateChar[%d] %p", CustomerID, this);
	}

	int		Exec();
	void		OnSuccess();
};

class CJobChangeBackpack : public CAsyncApiJob
{
  public:
	DWORD		BackpackID;
	int		BackpackSize;
	std::vector<wiInventoryItem> DroppedItems;
  
  public:
	CJobChangeBackpack(const obj_ServerPlayer* plr) : CAsyncApiJob(plr)
	{
		sprintf(desc, "CJobChangeBackpack[%d] %p", CustomerID, this);
	}

	int		Exec();
	void		OnSuccess();
};


class CJobAddLogInfo : public CAsyncApiJob
{
  public:
	int		CheatID;	// or 0 for normal logging messages
	int		GameServerId;
	DWORD		IP;

	char		Gamertag[128];
	char		Msg[2048];
	char		Data[4096];
  
  public:
	CJobAddLogInfo() : CAsyncApiJob()
	{
		sprintf(desc, "CJobAddLogInfo"); // do not modify - it's used for reentrancy checks
		Gamertag[0] = 0;
		GameServerId = 0;
	}

	int		Exec();
	void		OnSuccess() {}
};

class CJobMarketBuyItem : public CAsyncApiJob
{
public:
	CJobMarketBuyItem(int in_peerId);

	int	Exec();
	void OnSuccess();

	DWORD ItemId;
	BYTE BuyIdx;

private:
	int Result;
};
