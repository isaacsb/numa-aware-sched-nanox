#include "schedule.hpp"
#include "depsregion.hpp"
#include "wddeque.hpp"
#include "hashmap.hpp"
#include "plugin.hpp"
#include "system.hpp"
#include "config.hpp"
#include "os.hpp"

#include "numaif.h"

extern "C" {
#include "kgggp.h"
#include "ext/scotch.h"
}

#include <map>
#include <vector>
#include <cassert>
#include <queue>
#include <sstream>
#include <iomanip>

#include <cstring>
#include <cerrno>

#include <cstdio>
#include <utility>
#include <tr1/random>

#define NANOS_SCHED_PWA_RAISE_EVENT(x)  NANOS_INSTRUMENT( \
					   sys.getInstrumentation()->raiseOpenBurstEvent ( sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey( "partitioning-window" ), (x) ); )

#define NANOS_SCHED_PWA_CLOSE_EVENT     NANOS_INSTRUMENT( \
					    sys.getInstrumentation()->raiseCloseBurstEvent ( sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey( "partitioning-window" ), 0 ); )

namespace nanos {
  namespace ext {
    
    class P_MetapartModes
    {
    public:
      enum MetapartMode {
	ERROR=-1,
	WINDOW=0,
	DEPENDENCY,
	PROPAGATE
      };
    private:
      std::map<std::string, MetapartMode> _allowed;
      const std::string _deflt;
    public:
      P_MetapartModes()
	: _deflt("dependency")
      {
	_allowed["window"] = WINDOW;
	_allowed["dependency"] = DEPENDENCY;
	_allowed["propagate"] = PROPAGATE;
      }

      inline bool valid(const std::string &s) const
      {
	return _allowed.find(s) != _allowed.end();
      }

      inline MetapartMode value(const std::string &s) const
      {
	return _allowed.find(s)->second;
      }

      inline void defaultMode(std::string & s) const
      {
	s = _deflt;
      }

      inline std::string validList() const
      {
	std::ostringstream aux;
	std::map<std::string, MetapartMode>::const_iterator it = _allowed.begin();

	if (it != _allowed.end()) {
	  aux << it->first;
	  for (++it; it != _allowed.end(); ++it) {
	    aux << ", " << it->first;
	  }
	}

	return aux.str();
      }
    };

    typedef P_MetapartModes::MetapartMode MetapartMode;

    template <typename _WDQueue>
    class MetapartSchedPolicy : public SchedulePolicy {
    private:
      struct P_DepSock {
	int _socket;
	
	P_DepSock(int socket=0)
	  : _socket(socket)
	{}
	
	P_DepSock(const P_DepSock &o)
	  : _socket(o._socket)
	{}

	P_DepSock & force(int newSocket) {
	  _socket = newSocket;
	  return *this;
	}
	
	P_DepSock &operator=(int newSocket) {
	  if (_socket == 0 or (_socket < 0 and newSocket > 0))
	    _socket = newSocket;
	  
	  return *this;
	}
	
	inline operator int() const {
	  return std::abs(_socket);
	}
      };

      struct WDData : public ScheduleWDData
      {
	int id;
	WDData(int id_=0)
	  : id(id_)
	{}

	~WDData()
	{}

	static WDData & get(WD & wd)
	{
	  return *dynamic_cast<WDData*>( wd.getSchedulerData() );
	}
      };
      
    private:
      MetapartMode _mode;
      int _lastWd;
      bool _oldDep;
      int _wSize;
      int _wIntersect;
      int _wExtra;
      bool _steal;
      bool _stealOpt;
      bool _movePages;
      int _numSockets;
      Atomic<int> _partitioned;
      _WDQueue *_readyQueues;
      std::tr1::mt19937 _rand;
      double _unbalance;
      bool _checkGraph;
      bool _pauseThreads;
      std::string _archFilename;
      std::string _partMapFilename;
      std::string _distanceMatFilename;
      std::string _graphFilename;
      std::vector<int> _partMap;

      Atomic<int> _lastVirtId;

      int _currentBase;
      int _nextBase;
      int _lastPartitioned;

      Atomic<int> _updatingMap;
      Atomic<int> _readingMap;
      
      Atomic<bool> _metapart;
      int _scheduled;

      std::map< const void *, std::vector<int> > _antipred;
      std::map< const void *, int > _oldPred;
      std::map< const void *, int > _pred;
      std::map< int, std::map<int, size_t> > _deps;
      std::map< int, std::map<int, size_t> >_invdeps;

      std::list<WorkDescriptor *> _tmpQueue;
      HashMap< const void *, P_DepSock > _dependencySocket;
      std::vector<int> _taskSocket;
      
      Lock _tmpQueueLock;
      Lock _graphLock;
      Lock _taskSocketLock;
      Lock _partitionQueueLock;
      Lock _metapartLock;

      SCOTCH_Graph *_g;
      SCOTCH_Strat *_strat;
      SCOTCH_Arch *_arch;

      std::vector<SCOTCH_Num> _velotab;
      std::vector<SCOTCH_Num> _verttab;
      std::vector<SCOTCH_Num> _edlotab;
      std::vector<SCOTCH_Num> _edgetab;

      typedef DependableObject::TargetVector::const_iterator _tvIt;
      typedef DependableObject::DependableObjectVector::const_iterator _dovIt;
      
    public:
 
      MetapartSchedPolicy(MetapartMode mode,
			  bool oldDep,
			  int wSize,
			  int wIntersect,
			  int wExtra,
			  bool steal,
			  bool movePages,
			  int seedSCOTCH,
			  int seedPRNG,
			  double unbalance,
			  bool checkGraph,
			  bool pauseThreads,
			  std::string archFile,
			  std::string partMapFile,
			  std::string distanceMatFile,
			  std::string graphFile)
	: SchedulePolicy( "Metapart" ),
	  _mode(mode),
	  _lastWd(0),
	  _oldDep(oldDep),
	  _wSize(wSize),
	  _wIntersect(wIntersect),
	  _wExtra(wExtra),
	  _steal(false),
	  _stealOpt(steal),
	  _movePages(movePages and mode == P_MetapartModes::WINDOW),
	  _numSockets(0),
	  _partitioned(-1),
	  _rand(seedPRNG),
	  _unbalance(unbalance),
	  _checkGraph(checkGraph),
	  _pauseThreads(pauseThreads),
	  _archFilename(archFile),
	  _partMapFilename(partMapFile),
	  _distanceMatFilename(distanceMatFile),
	  _graphFilename(graphFile),
	  _currentBase(1),
	  _lastPartitioned(0),
	  _updatingMap(0),
	  _readingMap(0),
	  _metapart(false),
	  _scheduled(0)
      {
	debug0( "Using metapart schedule policy" );
	SCOTCH_randomSeed( seedSCOTCH );
	_g = SCOTCH_graphAlloc();
	SCOTCH_graphInit(_g);
	_lastVirtId = 0;

	if (mode == P_MetapartModes::WINDOW)
	  _wSize += wExtra;

	_updatingMap = 0;
      }

      virtual ~MetapartSchedPolicy()
      {
	SCOTCH_graphExit(_g);
	SCOTCH_memFree(_g);

	SCOTCH_stratExit(_strat);
	SCOTCH_memFree(_strat);

	SCOTCH_archExit(_arch);
	SCOTCH_memFree(_arch);
      }

    private:
      inline int _chooseNextSocket() {
	return _rand()%_numSockets;
      }

      inline int _chooseSocket( WD * wd, const std::string & from )
      {
	DOSubmit * doS = wd->getDOSubmit();
	if (doS == NULL) {
	  return _chooseNextSocket();
	}

	int virtId = WDData::get(*wd).id;

	if (virtId <= _lastPartitioned) {
	  while (true) {
	    // warning("Want to read b");
	    while (_updatingMap > 0);
	    
	    ++_readingMap;
	    // warning("Probably got it b");
	    if (_updatingMap > 0)
	      --_readingMap;
	    else
	      break;
	  }
	  // warning("Able to read b");
	  int chosen = -1;
	  int n = _taskSocket.size();
	  if (virtId < n) {
	    chosen = _taskSocket[virtId];
	  }
	  --_readingMap;

	  if (chosen != -1) {
	    return _partMap[chosen];
	  }

	  if (this->_mode == P_MetapartModes::WINDOW) {
	    warning( "Did not choose socket from partition for task " << wd->getId() << "(" << virtId << "), last: " << _lastPartitioned << ", from " << from );
	  }
	}

	std::vector<unsigned long long> _helper(_numSockets + 1, 0);
	if (this->_mode == P_MetapartModes::PROPAGATE) {
	  while (true) {
	    // warning("Want to read");
	    while (_updatingMap > 0);
	    
	    ++_readingMap;
	    // warning("Probably got it");
	    if (_updatingMap > 0)
	      --_readingMap;
	    else
	      break;
	  }
	  // warning("Able to read");
	  int n = _taskSocket.size();
	  const std::map<int, size_t> &parents = _invdeps[virtId];
	  for (std::map<int, size_t>::const_iterator it = parents.begin();
	       it != parents.end();
	       ++it) {
	    int parent_socket = (it->first < n) ? _taskSocket[it->first] : -1;
	    int currentSocket = parent_socket + 1;
	    _helper[currentSocket] += it->second;
	  }
	  
	  --_readingMap;
	}
	else {
	  const DependableObject::TargetVector &rt = doS->getReadTargets();
	  for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
	    const void *dep = (*it)->getAddress();
	    int currentSocket = _dependencySocket[dep];
	    size_t size = (static_cast<DepsRegion *>(*it))->getSize();
	    _helper[currentSocket] += size;
	  }

	  const DependableObject::TargetVector &wt = doS->getWrittenTargets();
	  for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
	    const void *dep = (*it)->getAddress();
	    int currentSocket = _dependencySocket[dep];
	    size_t size = (static_cast<DepsRegion *>(*it))->getSize();
	    _helper[currentSocket] += size;
	  }
	}

	int count = 0;
	unsigned long long best = 0;
	int argbest = 0;
	for (int i = 0; i <= _numSockets; ++i) {
	  unsigned long long current = _helper[i];
	  if (count == 0 or current > best) {
	    best = current;
	    argbest = i;
	    count = 1;
	  }
	  else if (current == best) {
	    ++count;
	    if (_rand()%count == 0)
	      argbest = i;
	  }
	}

	if (argbest == 0) {
	  argbest = _chooseNextSocket();
	}
	else
	  --argbest;

	return argbest;
      }

      void _updateDepMaps( WD * wd, int socket, bool final, int physnode=0 )
      {
	DOSubmit *doS = wd->getDOSubmit();
	if (doS == NULL) {
	  return;
	}

	switch (this->_mode) {
	case P_MetapartModes::WINDOW:
	  {
	    if (final) {
	      while (true) {
		while (_readingMap > 0);
		
		++_updatingMap;
		// warning("Acquiring map");
		if (_updatingMap > 1) {
		  --_updatingMap;
		  // warning("Did not acquire map " << _updatingMap.value());
		}
		else {
		  // warning("Is someone reading?");
		  while (_readingMap > 0);
		  // warning("Nobody is reading");
		  break;
		}
	      }

	      int n = _taskSocket.size();
	      int virtId = WDData::get(*wd).id;
	      if (n <= virtId) {
		_taskSocket.resize(virtId + 1 + _wSize, -1);
	      }

	      _taskSocket[virtId] = socket;
	      --_updatingMap;
	      // warning("Released map: " << _updatingMap.value() << " " << _readingMap.value());
	    }

	    socket = _partMap[socket];
	  }
#if defined(__GNUC__) && __GNUC__ > 6
          __attribute__ ((fallthrough)); // important for GCC 7 !!
#endif
	case P_MetapartModes::DEPENDENCY:
	  {
	    socket = socket + 1;
	    if (not final)
	      socket = -socket;
	    const DependableObject::TargetVector &rt = doS->getReadTargets();
	    for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
	      const void *dep = (*it)->getAddress();
	      _dependencySocket[dep] = socket;
	    }

	    const DependableObject::TargetVector &wt = doS->getWrittenTargets();
	    if (not final or not this->_movePages or this->_mode != P_MetapartModes::WINDOW) {
	      for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
		const void *dep = (*it)->getAddress();
		_dependencySocket[dep] = socket;
	      }
	    }
	    else if (this->_mode == P_MetapartModes::WINDOW) {
	      NANOS_SCHED_PWA_RAISE_EVENT(5);
	      std::vector<void *> move_list;
	      size_t page_size = sysconf(_SC_PAGESIZE);
	      for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
		const char *first = static_cast<const char *>((*it)->getAddress());
		debug("Current socket is " << _dependencySocket[first]._socket << ", new is " << socket);
		if (_dependencySocket[first]._socket > 0
		    and _dependencySocket[first]._socket != socket) {
		  size_t size = (static_cast<DepsRegion *>(*it))->getSize();
		  const char *last = first + size;
		  for (const char * dep(reinterpret_cast<const char *>(reinterpret_cast<size_t>(first) & ~(page_size-1)));
		       dep < last; dep += page_size) {
		    move_list.push_back(reinterpret_cast<void *>(reinterpret_cast<size_t>(dep)));
		  }
		}
		_dependencySocket[reinterpret_cast<const void *>(first)].force(socket);
	      }
	      
	      if (not move_list.empty()) {
		NANOS_INSTRUMENT(static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("partitioning-moved-pages"););
		NANOS_INSTRUMENT(sys.getInstrumentation()->raiseOpenBurstEvent( key, move_list.size() ););
		std::vector<int> node_list(move_list.size(), physnode);
		std::vector<int> status_list(move_list.size());
		long status = move_pages(0,
					 move_list.size(),
					 &move_list[0],
					 &node_list[0],
					 &status_list[0],
					 MPOL_MF_MOVE);
		debug("Moving " << move_list.size() << " pages!");
		NANOS_INSTRUMENT(sys.getInstrumentation()->raiseCloseBurstEvent( key, 0););
		if (status != 0) {
		  char buf[256];
		  int myerrno = errno;
		  char * errmsg = strerror_r(myerrno, buf, sizeof(buf));
		  warning( "Error moving pages to " << physnode << ": " << errmsg );
		}
	      }
	      else {
		debug( "No pages to move" )
	      }
	      NANOS_SCHED_PWA_CLOSE_EVENT;
	    }
	  }
	  break;
	case P_MetapartModes::PROPAGATE:
	  {
	    if (final) {
	      while (true) {
		while (_readingMap > 0);
		++_updatingMap;
		// warning("Acquiring map");
		if (_updatingMap > 1) {
		  --_updatingMap;
		  // warning("Did not acquire map " << _updatingMap.value());
		}
		else {
		  // warning("Is someone reading?");
		  while (_readingMap > 0);
		  // warning("Nobody is reading");
		  break;
		}
	      }

	      int n = _taskSocket.size();
	      int virtId = WDData::get(*wd).id;
	      if (n <= virtId) {
		_taskSocket.resize(virtId + 1 + _wSize, -1);
	      }

	      _taskSocket[virtId] = socket;
	      --_updatingMap;
	      // warning("Released map: " << _updatingMap.value() << " " << _readingMap.value());
	    }
	  }
          break;
	default:
	  break;
	}
      }

    public:
      virtual void queue ( BaseThread *thread, WD &wd )
      {
	int vNode = -1;
	int virtId = WDData::get(wd).id;
	
	switch( wd.getDepth() ) {
	case 0: // Hideous implicit tasks...
	  vNode = _numSockets;
	  if (virtId < 0)
	    vNode = _numSockets + 1;
	  break;
	case 1:
	  if (virtId == 0)
	    vNode = _numSockets;
	  else if (virtId < 0)
	    vNode = _numSockets + 1;
	  else if (_partitioned > 0 and _mode != P_MetapartModes::WINDOW) {
	    vNode = this->_chooseSocket( &wd, "queue dependency" );
	  }
	  else if (_lastPartitioned < virtId) {
	    SyncLockBlock block(_tmpQueueLock);
	    if ((_partitioned > 0 and _mode != P_MetapartModes::WINDOW)
		or _lastPartitioned >= virtId) {
	      vNode = this->_chooseSocket( &wd, "queue post-part" );
	    }
	    else {
	      _tmpQueue.push_back(&wd);
	    }
	  }
	  else {
	    vNode = this->_chooseSocket( &wd, "queue" );
	  }

	  break;
	default:
	  fatal0( "Not prepared for programs with nested tasks" );
	}

// #ifdef NANOS_DEBUG_ENABLED
// 	warning( "Put " << virtId << " in queue " << vNode );
// #endif
	
	if (vNode != -1) {
	  if (vNode < _numSockets)
	    _updateDepMaps( &wd, vNode, false );
	  
	  _readyQueues[vNode].push_back( &wd );
	}
      }

      virtual void queue ( BaseThread ** threads, WD ** wds, size_t numElems )
      {
        SchedulePolicy::queue(threads, wds, numElems);
      }

      virtual WD *atBeforeExit( BaseThread *thread, WD &wd, bool schedule )
      {
	if (_steal != _stealOpt and WDData::get(wd).id >= _lastPartitioned)
	  _steal = _stealOpt;
	
// #ifdef NANOS_DEBUG_ENABLED
// 	if (_mode == P_MetapartModes::WINDOW) {
// 	  int virtId = wd.getId() - _first;
// 	  warning( "Finished executing " << virtId );
// 	}
// #endif
	return SchedulePolicy::atBeforeExit(thread, wd, schedule);
      }

      virtual WD *atSubmit( BaseThread *thread, WD &newWD )
      {
	queue( thread, newWD );

	return 0;
      }

      virtual WD *atIdle( BaseThread *thread, int numSteals )
      {
	int vNode = _numSockets + 1;
	unsigned node = thread->runningOn()->getNumaNode();
	WorkDescriptor *wd = _readyQueues[vNode].pop_front( thread );
	
	if (wd == NULL) {
	  vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );
	  wd = _readyQueues[vNode].pop_front( thread );
	  
	  if (wd == NULL and _steal and numSteals > 0) {
	    vNode = _rand()%_numSockets;
	    wd = _readyQueues[vNode].pop_front( thread );
	  }
	  
	  // if (wd == NULL and this->_mode == P_MetapartModes::WINDOW) {
	  //   SyncLockBlock block(_tmpQueueLock);
	  //   if (not _tmpQueue.empty()) {
	  //     wd = _tmpQueue.front();
	  //     _tmpQueue.pop();
	  //     int newvNode = _chooseSocket(wd, "hungry-window");
	  //     if (newvNode != vNode) {
	  // 	_readyQueues[newvNode].push_back( wd );
	  // 	wd = NULL;
	  //     }
	  //   }
	  // }
	  
	  if (wd == NULL) {
	    vNode = _numSockets;
	    wd = _readyQueues[vNode].pop_front( thread );
	  }
	}

	if (wd != NULL and vNode < _numSockets) {
	  vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );
// 	  if (this->_mode == P_MetapartModes::WINDOW)
// 	    vNode = thread->getId();
	  _updateDepMaps( wd, vNode, true, node );
	  // int id = wd->getId();
	  // warning("Got to execute task " << id);
	}
	
	return wd;
      }

    public:

      virtual void atCreate( DependableObject &depObj )
      {
	DOSubmit *dp = (DOSubmit *) &depObj;
	if (dp == NULL) {
	  return;
	}

	WD *wd = (WD *) dp->getRelatedObject();
	if (wd->getDepth() == 0) {
	  return;
	}

	if (_implicit(*wd))
	  return;

	if (WDData::get(*wd).id != 0)
	  return;
	NANOS_SCHED_PWA_RAISE_EVENT(4);
	int virtId = ++_lastVirtId;
	
	WDData::get(*wd).id = virtId;

	_graphLock.acquire();
	if (_partitioned > 0 and _mode == P_MetapartModes::DEPENDENCY) {
	  _graphLock.release();
	  NANOS_SCHED_PWA_CLOSE_EVENT;
	  return;
	}
	
	// Add missing elements to the basic graph structure
	if (_deps.find(virtId) == _deps.end()) {
	  for (int i = _lastWd + 1; i <= virtId; ++i) {
	    _deps[i].size(); // ensure vertex creation
	    _invdeps[i].size();
	  }
	  
	  _lastWd = virtId;
	}
	
	
	const DependableObject::TargetVector &rt = dp->getReadTargets();
	for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
	  const void *dep = (*it)->getAddress();
	  size_t size = (static_cast<DepsRegion *>(*it))->getSize();
	  int o_parent = _oldPred[dep];
	  if (_oldDep and o_parent != 0) { // If 0, nobody has written to it yet
	    if (_deps[o_parent].find(virtId) == _deps[o_parent].end()) {
	      _deps[o_parent].insert(std::make_pair(virtId, size));
	      _invdeps[virtId].insert(std::make_pair(o_parent, size));
	    }
	  }

	  int parent = _pred[dep];
	  if (parent != 0 and parent != o_parent) { // If 0, nobody has written to it yet
	    if (_deps[parent].find(virtId) == _deps[parent].end()) {
	      _deps[parent].insert(std::make_pair(virtId, size));
	      _invdeps[virtId].insert(std::make_pair(parent, size));
	    }
	  }

	  _antipred[dep].push_back(virtId); // For antidependencies
	}

	const DependableObject::TargetVector &wt = dp->getWrittenTargets();
	for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
	  const void *dep = (*it)->getAddress();
	  size_t size = (static_cast<DepsRegion *>(*it))->getSize();
	  int o_parent = _oldPred[dep];
	  if (_oldDep and o_parent != 0 and o_parent != virtId) {
	    if (_deps[o_parent].find(virtId) == _deps[o_parent].end()) {
	      _deps[o_parent].insert(std::make_pair(virtId, size));
	      _invdeps[virtId].insert(std::make_pair(o_parent, size));
	    }
	  }
	  
	  int parent = _pred[dep];
	  if (parent != 0 and parent != virtId and parent != o_parent) { // If -1, nobody has written to it yet
	    if (_deps[parent].find(virtId) == _deps[parent].end()) {
	      _deps[parent].insert(std::make_pair(virtId, size));
	      _invdeps[virtId].insert(std::make_pair(parent, size));
	    }
	  }

	  std::vector<int> &v = _antipred[dep];
	  int n = v.size();
	  for (int i = 0; i < n; ++i) {
	    _deps[v[i]].insert(std::make_pair(virtId, size));
	    _invdeps[virtId].insert(std::make_pair(v[i], size));

	  }
	  v.clear();
	  

	  if (_pred[dep] != virtId) {
	    _oldPred[dep] = _pred[dep];
	  }
	  _pred[dep] = virtId;
	}
	_graphLock.release();
	
	if (virtId + 1 >= _currentBase + _wSize and _partitioned < _currentBase) {
	  SyncLockBlock qlock(_partitionQueueLock);
	  if (_partitioned <= 0 and _scheduled < _currentBase) {
	    this->_submitPartition(virtId);
	    if (this->_mode == P_MetapartModes::WINDOW)
	      _wSize = _wSize - _wExtra;
	  }
	  else if (this->_mode == P_MetapartModes::WINDOW) {
	    int next = std::max(0, _scheduled - _wIntersect + 1 + _wSize);
	    if (virtId + 1 >= next) {
	      this->_submitPartition(virtId);
	    }
	  }
	}
	NANOS_SCHED_PWA_CLOSE_EVENT;
      }

      void _submitPartition(int virtId)
      {
	_scheduled = virtId;
	nanos_smp_args_t transformer = { (void (*)(void *))(void (*)(P_PartitionerArgs &)) _callPartitioner };
	nanos_device_t dev = {local_nanos_smp_factory, &transformer};
	P_PartitionerArgs *args = NULL;
	WD * graphWd = NULL;
	sys.createWD(&graphWd, 1, &dev, sizeof(P_PartitionerArgs &), __alignof__(P_PartitionerArgs), (void **) &args, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, "@@partitioner", NULL);
	graphWd->setImplicit( true );
	::nanos_region_dimension_t dimension;

	dimension.size = sizeof(SCOTCH_Graph);
	dimension.lower_bound = 0L;
	dimension.accessed_length = 1L * sizeof(SCOTCH_Graph);

	
	DataAccess deps(this->_g, true, true, false, false, false, 1, &dimension, 0L);

	if (graphWd == NULL) {
	  fatal0( "Could not create WorkDescriptor for graph metapart. Exiting..." );
	}

	args->scheduler = this;
	args->lastTask = virtId;
	WDData::get(*graphWd).id = -virtId;
	
	sys.submitWithDependencies(*graphWd, 1, &deps);
      }

      static void * local_nanos_smp_factory( void *argTransform )
      {
	nanos_smp_args_t *smp = reinterpret_cast<nanos_smp_args_t *>(argTransform);
	return ( void * )new ext::SMPDD( smp ? smp->outline : NULL );
      }

      virtual WD * atBlock(BaseThread * thread, WD * current)
      {
        atWait(thread);
        return SchedulePolicy::atBlock(thread, current);
      }

      virtual void atWait( BaseThread *thread )
      {
	SyncLockBlock qlock(_partitionQueueLock);
	if (_partitioned <= 0 or (this->_mode == P_MetapartModes::WINDOW and _scheduled < _lastWd)) {
	  this->_submitPartition(_lastWd);
	}
      }

      virtual bool testDequeue()
      {
        return (sys.getReadyNum() > 0) or (_lastVirtId > _lastPartitioned);
      }

    private:

      inline bool _implicit (const WD & wd) const
      {
	return wd.getId() <= sys.getNumThreads() + 1;
      }

      struct P_PartitionerArgs
      {
	MetapartSchedPolicy<_WDQueue> *scheduler;
	int lastTask;
      };

      static void _callPartitioner(P_PartitionerArgs & p)
      {
	p.scheduler->_doPartition(p.lastTask);
      }

      inline void _doPartition(int lastTask)
      {
	SyncLockBlock block( _metapartLock );
	if (_lastPartitioned >= lastTask)
	  return;
	
	int num_vertices = lastTask - _currentBase + 1;
	if (_partitioned < 1 and _pauseThreads) {
	  sys.stopScheduler();
	  sys.waitUntilThreadsPaused();
	}

	debug( "Doing partition from " << _currentBase << " to " << lastTask);

	_nextBase = std::max(_currentBase + 1, lastTask - _wIntersect + 1);
	NANOS_SCHED_PWA_RAISE_EVENT(1);
	{
	  //warning("GRPHLOCK_partition2");
	  SyncLockBlock block2(_graphLock);
	  //warning("GRPHLOCK_partition2 acquired");
	  this->_buildGraph(num_vertices);
	}
	NANOS_SCHED_PWA_CLOSE_EVENT;
	NANOS_SCHED_PWA_RAISE_EVENT(2);
	this->_partitionGraph(num_vertices);
	NANOS_SCHED_PWA_CLOSE_EVENT;

	_lastPartitioned = lastTask;
	NANOS_SCHED_PWA_RAISE_EVENT(3);
	//warning("TMPQLOCK_partition2");
	SyncLockBlock qlock(_tmpQueueLock);
	//warning("TMPQLOCK_partition2 acquired");
	std::list<WD *>::iterator it = _tmpQueue.begin();
	while (it != _tmpQueue.end()) {
	  WD *wd = *it;
	  if (WDData::get(*wd).id > lastTask) {
	    ++it;
	    continue;
	  }
	  
	  it = _tmpQueue.erase(it);
	  int socket = this->_chooseSocket(wd, "doPartition");
	  this->_updateDepMaps(wd, socket, false);
	  _readyQueues[socket].push_back(wd);
	}
	NANOS_SCHED_PWA_CLOSE_EVENT;
	if (_partitioned < 1) {
	  if (_pauseThreads) {
	    sys.startScheduler();
	    sys.waitUntilThreadsUnpaused();
	  }
	}

#ifdef NANOS_DEBUG_ENABLED
	if (_graphFilename != "") {
	  std::ostringstream a;
	  a << _graphFilename << "-" << _currentBase << ".grf";
	  std::FILE * foutput = std::fopen(a.str().c_str(), "w");
	  ::SCOTCH_graphSave(_g, foutput );
	  std::fclose(foutput);
	}	
#endif
	
	_partitioned = _currentBase;
	_currentBase = _nextBase;

	debug( "Finished partition" );
      }

      void _partitionGraph(int num_vertices)
      {
	debug( "In metapart procedure..." );
        debug( "Doing partition from " << _currentBase << " reaching " << num_vertices << " tasks");

	std::vector<SCOTCH_Num> initial_partition( num_vertices, 0 );
	std::vector<SCOTCH_Num> output_partition( num_vertices, 0 );
	std::vector<SCOTCH_Num> mig_costs( num_vertices, 1 );


	// Lock for updating map
	{
	  debug ("Value of reading map: " << _readingMap.value() );
	  while (true) {
	    while (_readingMap > 0);
	    ++_updatingMap;
	    if (_updatingMap > 1)
	      --_updatingMap;
	    else {
	      while (_readingMap > 0);
	      break;
	    }
	  }
	  // warning("Map is locked");
	}

#ifdef NANOS_DEBUG_ENABLED
	int count = 0;
#endif
	SCOTCH_Num archSize = SCOTCH_archSize(_arch);
	int nextPart = 0;
	int n = _taskSocket.size();
	for (int it = _currentBase; it < n; ++it) {
	  if (it < _currentBase + num_vertices) {
	    if (_taskSocket[it] >= 0) {
#ifdef NANOS_DEBUG_ENABLED
	      ++count;
#endif
	      initial_partition[it - _currentBase] = _taskSocket[it];
	      mig_costs[it - _currentBase] = 10000;
	      output_partition[it - _currentBase] = _taskSocket[it];
	    }
	    else {
	      initial_partition[it - _currentBase] = nextPart;
	      mig_costs[it - _currentBase] = 1;
	      output_partition[it - _currentBase] = -1; // nextPart;
	      nextPart = (nextPart + 1)%archSize;
	    }
	  }
	}
	--_updatingMap;

#ifdef NANOS_DEBUG_ENABLED
	if (_taskSocket.size() >= (unsigned) _wIntersect and count != _wIntersect) {
	  warning( "The number of tasks (" << count << ") retreived from base " << _currentBase << " is not the window intersection (" << _wIntersect << ")" );
	}
#endif
	if (_partitioned > 0) {
	  debug( "Setting to zero from " << n << " to " << _currentBase + num_vertices );
	  for (int it = _lastPartitioned + 1 - _currentBase; it < num_vertices; ++it) {
	    initial_partition[it] = nextPart;
	    mig_costs[it] = 0;
	    output_partition[it] = -1;  // nextPart;
	    nextPart = (nextPart + 1)%archSize;
	  }

	  // SCOTCH_graphRemapFixed(_g,
	  // 			 _arch,
	  // 			 &initial_partition[0],
	  // 			 5.0,
	  // 			 &mig_costs[0],
	  // 			 _strat,
	  // 			 &output_partition[0]);
	  
 	  // SCOTCH_graphRemap(_g,
	  // 		    _arch,
	  // 		    &initial_partition[0],
	  // 		    5.0,
	  // 		    &mig_costs[0],
	  // 		    _strat,
	  // 		    &output_partition[0]);

	  // SCOTCH_graphMapFixed(_g,
	  // 		       _arch,
	  // 		       _strat,
	  // 		       &output_partition[0]);

	  SCOTCH_graphPartFixed(_g,
	  			archSize,
	  			_strat,
	  			&output_partition[0]);

	  // SCOTCH_graphMap(_g,
	  // 		  _arch,
	  // 		  _strat,
	  // 		  &output_partition[0]);
	  
	  // int unmoved = 0;
	  // int moved = 0;
	  // for (int i = _lastPartitioned - _currentBase + 1; i < num_vertices; ++i) {
	  //   if (initial_partition[i] == output_partition[i]) {
	  //     ++unmoved;
	  //   }
	  //   else
	  //     ++moved;
	  // }
	  // warning( "Left " << unmoved << " unmoved, moved a total of " << moved );
	}
	else {
	  warning ( "First partition: a total of " << num_vertices << " from " << _currentBase );
	  SCOTCH_graphMap(_g,
			  _arch,
			  _strat,
			  &output_partition[0]);

	  if (this->_mode == P_MetapartModes::WINDOW) {
	    SCOTCH_stratExit(_strat);
	    SCOTCH_stratInit(_strat);
   
	    std::string mystrat = this->getStratString(_unbalance, archSize);
	    SCOTCH_stratGraphMap(_strat,
	    			 mystrat.c_str());

	    // SCOTCH_stratGraphMapBuild(_strat,
	    // 			      SCOTCH_STRATDEFAULT|SCOTCH_STRATREMAP|SCOTCH_STRATSPEED,
	    // 			      SCOTCH_archSize(_arch),
	    // 			      _unbalance);
	  }
	}
	  
	{
	  debug ("Value of reading map: " << _readingMap.value() );
	  while (true) {
	    while (_readingMap > 0);
	    ++_updatingMap;
	    if (_updatingMap > 1)
	      --_updatingMap;
	    else {
	      while (_readingMap > 0);
	      break;
	    }
	  }
	  // warning("Map is locked");
	}
        n = _taskSocket.size();
	if (n <= _lastPartitioned + num_vertices) {
	  n = _lastPartitioned + num_vertices + 1;
	  _taskSocket.resize(n, -1);
	}
	
	for (int i = _lastPartitioned + 1 - _currentBase; i < num_vertices; ++i) {
#ifdef NANOS_DEBUG_ENABLED
	  if (output_partition[i] >= 0) { // should always be true
	    int current = _taskSocket[i + _currentBase];
	    if (current != -1) {
	      if (current != output_partition[i]) {
	    	warning( "Partition for task " << i + _currentBase << " has not been preserved" );
	      }
	    }
#endif
	    if (_taskSocket[i + _currentBase] == -1) {
	      _taskSocket[i + _currentBase] = output_partition[i];
	    }
#ifdef NANOS_DEBUG_ENABLED
	  }
	  else {
	    warning( "Task " << i + _currentBase << " has not been partitioned" );
	  }
#endif
	  
	}

	--_updatingMap;
	debug( "Finished metapart graph");
      }

      std::string getStratStringZZ(double imbalance, int nparts, const std::string & env)
      {
#define MAXSTR 8192
#define NREFPASS 10
#define NINITPASS 4
#define MAXNEGMOVE 300	
	int ninitpass, nrefpass, maxnegmove;
	ninitpass = NINITPASS;
	nrefpass = NREFPASS;
	maxnegmove = MAXNEGMOVE;
	
	char s[MAXSTR];
	snprintf (s, sizeof(s),
		  "z{bal=%lf,pass=%d,env=\"%s\"}f{bal=%lf,pass=%d,move=%d}", // initial part
		  imbalance,              // <- ubfactor
		  ninitpass,                                     // <- nb init pass
		  env.c_str(),                                   // <- env (initial partitioning)
		  imbalance,              // <- ubfactor	    
		  nrefpass,                                      // <- nb ref pass (or -1)
		  maxnegmove                                     // <- maxnegmove for refinement (or -1)    
		  );
  
	char ss[8192];
	snprintf (ss, sizeof(ss),
		  "m{vert=%d,low=%s,asc=f{bal=%lf,move=%d,pass=%d}}",
		  30*nparts,                    // <- max size of coarsest graph
		  s,                                          // <- KGGGP + FM refinement
		  imbalance,                             // <- ubfactor
		  maxnegmove,                                 // <- maxnegmove for refinement (or -1)	    	    
		  nrefpass                                    // <- nb of ref pass (or -1)
		  );

	std::string res(ss);
	return ss;

#undef MAXSTR
#undef NREFPASS
#undef NINITPASS
#undef MAXNEGMOVE
      }

      std::string getStratString(double imbalance, int nparts)
      {
#define MAXSTR 8192
#define NREFPASS 10
#define NINITPASS 4
#define MAXNEGMOVE 300
       
	int gainscheme, greedy, connectivity, useseeds, npass, multilevel;
  
	gainscheme = KGGGP_GAIN_CLASSIC;
	greedy = KGGGP_GREEDY_GLOBAL;
	connectivity = KGGGP_CONNECTIVITY_YES;
	useseeds = KGGGP_USE_SEEDS;
	// npass = NINITPASS;
	npass = NINITPASS;
	multilevel = 0;

	int maxnegmove = MAXNEGMOVE;
	// if(maxnegmove < 0) maxnegmove = INT_MAX;  
  
	char gain = 'c';
	if(gainscheme == KGGGP_GAIN_DIFF) gain = 'd';
	else if(gainscheme == KGGGP_GAIN_HYBRID) gain = 'h';  
  
	char grdy = 'g';
	if(greedy == KGGGP_GREEDY_LOCAL) grdy = 'l';
  
	char conn = 'n';
	if(connectivity == KGGGP_CONNECTIVITY_YES) conn = 'y';
  
	char seed = 'n';
	if(useseeds == KGGGP_USE_SEEDS) seed = 's'; else if(useseeds == KGGGP_USE_BUBBLES) seed = 'b';
  
	char s[MAXSTR];
	snprintf (s, sizeof(s),
		  "g{bal=%lf,gain=%c,seed=%c,greedy=%c,conn=%c,pass=%d}f{bal=%lf,pass=%d,move=%d}", // initial part
		  imbalance,           // <- ubfactor
		  gain,                                       // <- gain scheme
		  seed,                                       // <- use seeds (or not)
		  grdy,                                       // <- greedy approach (global or local)
		  conn,                                       // <- enforce connectivity
		  npass,                                      // <- nb of KGGGP passes
		  imbalance,           // <- ubfactor	    
		  NREFPASS,
		  maxnegmove                                  // <- maxnegmove for refinement (or -1)	    
		  );

	char * sss = s;

	if (multilevel) {
	  char ss[8192];
	  snprintf (ss, sizeof(ss),
		    "m{vert=%d,low=%s,asc=f{bal=%lf,move=%d,pass=%d}}",
		    30*nparts,                    // <- max size of coarsest graph
		    s,                                          // <- KGGGP + FM refinement
		    imbalance,                             // <- ubfactor
		    maxnegmove,                                 // <- maxnegmove for refinement (or -1)	    	    
		    NREFPASS                                    // <- max nb of pass (or -1)
		    );
  
	  sss = ss;
	}

	return std::string(sss);
	
#undef MAXSTR
#undef NREFPASS
#undef NINITPASS
#undef MAXNEGMOVE
      }

      // Build the graph for SCOTCH from the adjacency lists
      inline void _buildGraph(SCOTCH_Num vertnbr)
      {
	SCOTCH_graphFree(_g);
	int lastTask = vertnbr + _currentBase - 1;
	SCOTCH_Num baseval = 0;
	
	_verttab.clear();
        _edgetab.clear();
        _edlotab.clear();
	_verttab.reserve(vertnbr+1);
	_verttab.push_back(baseval);
	_velotab.clear();

	for (int i = _currentBase; i <= lastTask; ++i) {
	  const std::map<int, size_t> &v1 = _deps[i];
	  for (std::map<int, size_t>::const_iterator it = v1.begin();
	       it != v1.end();
	       ++it) {
	    if (it->first >= _currentBase and it->first <= lastTask) {
	      _edgetab.push_back(static_cast<SCOTCH_Num>(it->first - _currentBase));
	      _edlotab.push_back(std::max(static_cast<SCOTCH_Num>(it->second/1024), (SCOTCH_Num) 1));
	    }
	  }

	  std::map<int, size_t> &v2 = _invdeps[i];
	  bool prev_old = true;
	  for (std::map<int, size_t>::iterator it = v2.begin();
	       it != v2.end();
	       ++it) {
#ifdef NANOS_DEBUG_ENABLED
	    if (_checkGraph and v1.find(it->first) != v1.end()) {
	      fatal0( "Something really dark is happening: there are circular dependencies" );
	    }
#endif

	    if (it->first >= _currentBase and it->first <= lastTask) {
	      _edgetab.push_back(static_cast<SCOTCH_Num>(it->first - _currentBase));
	      _edlotab.push_back(std::max(static_cast<SCOTCH_Num>(it->second/1024), (SCOTCH_Num) 1));
	    }

	    if (prev_old and it->first >= _nextBase) {
	      v2.erase(v2.begin(), it);
	      prev_old = false;
	    }
	  }

	  if (i < _nextBase) {
	    _deps.erase(_deps.find(i));
	    _invdeps.erase(_invdeps.find(i));
	  }

	  _verttab.push_back(_edgetab.size() + baseval);
#ifdef NANOS_DEBUG_ENABLED
	  if (_verttab[_verttab.size()-2] == _verttab[_verttab.size()-1]) {
	    warning ("Edges for " << i << " are nonexistent");
	  }
#endif
	}

	std::map< const void *, int >::iterator it_p = _pred.begin();
	while (it_p != _pred.end()) {
	  std::map< const void *, int >::iterator next_it = it_p;
	  ++next_it;
	  if (it_p->second < _nextBase)
	    _pred.erase(it_p);

	  it_p = next_it;
	}

	it_p = _oldPred.begin();
	while (it_p != _oldPred.end()) {
	  std::map< const void *, int >::iterator next_it = it_p;
	  ++next_it;
	  if (it_p->second < _nextBase)
	    _oldPred.erase(it_p);

	  it_p = next_it;
	}

	SCOTCH_graphBuild(_g,
			  baseval,
			  vertnbr,
			  &_verttab[0],
			  &_verttab[1],
			  NULL, // vertex loads
			  NULL, // vertex labels
			  _edgetab.size(),
			  &_edgetab[0],
			  &_edlotab[0]
			  );

#ifdef NANOS_DEBUG_ENABLED
	if (_checkGraph and SCOTCH_graphCheck(_g) != 0) {
	  fatal( "Error in graph check. Exiting..." );
	}
#endif
      }
      
      struct TeamData : public ScheduleTeamData
      {
	_WDQueue *_readyQueues;
	
	TeamData ( _WDQueue **readyQueues, int sockets ) : ScheduleTeamData()
	{
	  _readyQueues = NEW _WDQueue[sockets + 2];
	  *readyQueues = _readyQueues;
	}
	
	~TeamData () {
	  delete[] _readyQueues;
	}
      };
      
    public:
      virtual size_t getTeamDataSize () const { return sizeof(TeamData); }
      virtual size_t getThreadDataSize () const { return 0; }

      virtual ScheduleTeamData * createTeamData () {
	_numSockets = sys.getNumNumaNodes();
	ScheduleTeamData *tdata = NEW TeamData(&_readyQueues, _numSockets);

	_initInternalStructures();
	
	return tdata;
      }

      virtual ScheduleThreadData * createThreadData () { return NULL; }

      virtual size_t getWDDataSize () const { return sizeof( WDData ); }
      virtual size_t getWDDataAlignment () const { return __alignof__( WDData ); }
      virtual void initWDData ( void * data ) const
      {
	NEW (data)WDData();
      }

    private:
      void _initInternalStructures()
      {
    	_arch = SCOTCH_archAlloc();
    	SCOTCH_archInit(_arch);

	_strat = SCOTCH_stratAlloc();
	
    	SCOTCH_Num archSize = _numSockets;
    	if ( _archFilename != "" ) {
    	  std::FILE * archFile = std::fopen(_archFilename.c_str(), "r");

    	  if (archFile == NULL) {
    	    fatal0( "Could not open architecture description file" );
    	  }

    	  if (SCOTCH_archLoad(_arch, archFile) != 0) {
    	    fatal0( "Problem reading architecture description file" );
    	  }

    	  std::fclose(archFile);

    	  archSize = SCOTCH_archSize(_arch);
	  debug( "Read from architecture file, size " << archSize );
    	}
    	else {
	  std::vector<SCOTCH_Num> arch_verttab;
	  std::vector<SCOTCH_Num> arch_vendtab;
	  
	  std::vector<SCOTCH_Num> arch_edges, arch_distances;

	  const std::vector<int> &numanodes = sys.getNumaNodeMap();

	  std::string online_nodes_fname = "/sys/devices/system/node/online";
	  std::ifstream online_nodes(online_nodes_fname.c_str());

	  if (online_nodes.bad()) {
	    fatal0( "Could not open list of online nodes in the system, "
		    << "required to read the distances" );
	  }
	  
	  std::vector<int> physnodes;
	  char c;
	  do {
	    int l;
	    online_nodes >> l;
	    physnodes.push_back(l);
	    online_nodes >> std::ws;
	    c = online_nodes.peek();
	    if (c != ',') {
	      int r;
	      if (online_nodes >> c >> r)
	        for (int i = l+1; i <= r; ++i)
		  physnodes.push_back(i);
	    }
	    online_nodes >> std::ws;
	  } while (online_nodes >> c and c != '\0');
	  online_nodes.close();

	  int pnodes = physnodes.size();
	  if (pnodes != sys.getSMPPlugin()->getNumSockets()) {
	    fatal0( "The number of online nodes in the system and Nanos do not match" );
	  }
	  
	  int n = numanodes.size();
	  // pnodes = std::min(n, pnodes);
	  std::map<int, std::map<int, SCOTCH_Num> > myDist;

	  if (_distanceMatFilename == "") {
	    //warning0( "Using system distance matrix" );
	    for (int i = 0; i < n; ++i) {
	      if (numanodes[i] >= 0) { // node exists
		std::ostringstream fname;
		fname << "/sys/devices/system/node/node" << i << "/distance";
		std::ifstream distances(fname.str().c_str());
		if (distances.good()) {
		  SCOTCH_Num node_a = numanodes[i];
		  for (int j = 0; j < pnodes; ++j) {
		    SCOTCH_Num distance;
		    distances >> distance;
		    if (physnodes[j] >= n)
		      break;
		    SCOTCH_Num node_b = numanodes[physnodes[j]];
		    
		    if (node_a == node_b) {
		      myDist[node_a][node_b] = 1;
		    }
		    else if (node_b >= 0) {
		      // myDist[node_a][node_b] = distance;
		      myDist[node_a][node_b] = 1<<int(((distance+9)*2)/10);
		    }
		  }
		}
		else {
		  fatal0("Could not read distances for physical node " << i);
		}
	      }
	    }
	  }
	  else {
	    std::ifstream distances(_distanceMatFilename.c_str());
	    if (distances.good()) {
	      // warning0( "Reading distance file" );
	      for (int i = 0; i < pnodes; ++i) {
		if (physnodes[i] >= n)
		  break;
		SCOTCH_Num node_a = numanodes[physnodes[i]];
		// if (node_a >= 0)
		//   std::cerr << node_a << " (" << physnodes[i] << ")" << std::endl;
		for (int j = 0; j < pnodes; ++j) {
		  double distance;
		  distances >> distance;
		  if (physnodes[j] >= n)
		    continue;
		  SCOTCH_Num node_b = numanodes[physnodes[j]];
		  if (node_a == node_b and node_a >= 0) {
		      myDist[node_a][node_b] = 1;
		  }
		  else if (node_a >= 0 and node_b >= 0) {
		    // myDist[node_a][node_b] = distance;
		    myDist[node_a][node_b] = 1<<int(((distance+9)*2)/10);
		    //std::cerr << std::setw(4) << distance << ' ';
		  }
		}
		// if (node_a >= 0)
		//   std::cerr << std::endl;
	      }
	    }
	    else {
	      fatal0("Could not read distances from the distance file");
	    }
	  }

	  int numThreads = sys.getNumThreads();
	  archSize = _numSockets;
// 	  archSize = numThreads;
	  _partMap.resize(archSize, -1);
	  std::vector<SCOTCH_Num> arch_velotab(archSize, 0);
	  for (int i = 0; i < numThreads; ++i) {
	    BaseThread *t = sys.getWorker(i);
	    if (t != NULL) {
	      int node = numanodes[t->runningOn()->getNumaNode()];
	      ++arch_velotab[node];
// 	      _partMap[i] = node;
// 	      ++arch_velotab[i];
	    }
	    else {
	      fatal0("Problem with thread " << i <<": it seems not to exist");
	    }
	  }
 	  for (int i = 0; i < archSize; ++i) {
	    _partMap[i] = i;
	  }

	  arch_edges.reserve(archSize*(archSize-1));
	  arch_distances.reserve(archSize*(archSize-1));

	  for (int i = 0; i < archSize; ++i) {
	    arch_verttab.push_back(arch_edges.size());
	    for (int j = 0; j < archSize; ++j) {
	      if (i != j) {
		arch_edges.push_back(j);
		arch_distances.push_back(myDist[_partMap[i]][_partMap[j]]);
	      }
	    }
	    arch_vendtab.push_back(arch_edges.size());
	  }

	  SCOTCH_Graph *arch_graph = SCOTCH_graphAlloc();
	  SCOTCH_graphInit(arch_graph);
	  SCOTCH_graphBuild(arch_graph,
			    0,
			    archSize,
			    &arch_verttab[0],
			    &arch_vendtab[0],
			    &arch_velotab[0],
			    NULL,
			    arch_edges.size(),
			    &arch_edges[0],
			    &arch_distances[0]);

	  int status = SCOTCH_graphCheck(arch_graph);
	  if (status != 0) {
    	    fatal0( "Problem building the graph of the architecture" );
    	  }

	  SCOTCH_stratInit(_strat);
	  status = SCOTCH_archBuild(_arch,
				    arch_graph,
				    0,
				    NULL,
				    _strat);

	  if (status != 0) {
    	    fatal0( "Problem building the architecture from the graph" );
    	  }

	  SCOTCH_stratExit(_strat);
	  SCOTCH_graphExit(arch_graph);
	  SCOTCH_memFree(arch_graph);
    	}
        
    	std::ifstream mapFile(_partMapFilename.c_str());
    	if (not mapFile.is_open()) {
    	  if (_partMapFilename != "") {
    	    warning0( "Could not open the part map file. "
    		      << "Continuing with the identity map, cross your fingers...");
    	  }

	  if ( _archFilename != "" ) {
	    int final_parts = archSize;
	    if (final_parts != _numSockets) {
	      fatal0( "The final number of parts does not match the number of sockets!" );
	    }
	    _partMap.resize(archSize, -1);
	    for (int i = 0; i < archSize; ++i) {
	      _partMap[i] = i;
	    }
	  }
    	}
    	else {
	  debug( "Reading from map file" );
    	  int total_parts, final_parts;
    	  mapFile >> total_parts >> final_parts;

    	  if (final_parts != _numSockets) {
    	    fatal0( "The final number of parts does not match the number of sockets!" );
    	  }
	  
    	  if (total_parts != archSize) {
    	    fatal0( "The number of total parts in the part map file does not match the number of parts according to the architecture description!" );
    	  }
    	  _partMap.resize( total_parts, -1 );
	  
    	  int missing = total_parts;
    	  for (int s = 0; s < final_parts; ++s) {
    	    int cores_socket;
    	    mapFile >> cores_socket;
    	    for (int c = 0; c < cores_socket; ++c) {
    	      int core;
    	      mapFile >> core;
    	      if (core >= total_parts) {
    		fatal0( "Part " << core << " for socket " << s
    			<< " is out of the range [0, " << total_parts << ")" );
    	      }
    	      else if (_partMap[core] != -1) {
    		fatal0( "Part " << core << " is already in socket " << _partMap[core]
    			<< ", cannot assign it to socket " << s );
    	      }
    	      else {
    		_partMap[core] = s;
    		--missing;
    	      }
    	    }
    	  }
	  
    	  mapFile.close();

    	  if (missing != 0) {
    	    fatal0( "There are " << missing << " unassigned parts to sockets!" );
    	  }
    	}

    	SCOTCH_stratInit(_strat);
    	SCOTCH_stratGraphMapBuild(_strat,
    				  SCOTCH_STRATDEFAULT|SCOTCH_STRATQUALITY|SCOTCH_STRATRECURSIVE,// |SCOTCH_STRATSPEED,// |SCOTCH_STRATQUALITY,
    				  archSize,
    				  _unbalance);

	debug( "Initial strategy has been prepared" );
      }
      
    };

    class MetapartSchedPlugin : public Plugin {
    private:
      std::string _mode;
      bool _steal;
      bool _oldDep;
      int _wSize;
      int _wIntersect;
      int _wExtra;
      int _seedSCOTCH;
      int _seedPRNG;
      double _unbalance;
      bool _checkGraph;
      bool _pauseThreads;
      bool _usePriorities;
      bool _movePages;
      std::string _archFilename;
      std::string _partMapFilename;
      std::string _distanceMatFilename;
      std::string _graphFilename;
      
    public:
      P_MetapartModes metapartModes;
      
    public:
      MetapartSchedPlugin()
	: Plugin( "Metapart scheduling plugin", 1 ),
	  _steal(false),
	  _oldDep(false),
	  _wSize(128),
	  _wIntersect(64),
	  _wExtra(0),
	  _seedSCOTCH(time(NULL)),
	  _seedPRNG(time(NULL) + 1),
	  _unbalance(0.001),
	  _checkGraph(false),
	  _pauseThreads(false),
	  _usePriorities(false),
	  _movePages(false),
	  _archFilename(""),
	  _partMapFilename(""),
	  _distanceMatFilename("")
      {
	metapartModes.defaultMode(_mode);
      }
      
      
      virtual void config( Config &cfg )
      {
	cfg.setOptionsSection("Metapart scheduling", "Metapart scheduling module");

	cfg.registerConfigOption( "partitioning-mode", NEW Config::StringVar( _mode ), "Mode for metapart the rest of the graph. Allowed values: " + metapartModes.validList() + " (default: " + _mode + ")" );
	cfg.registerArgOption( "partitioning-mode", "partitioning-mode" );
	cfg.registerEnvOption( "partitioning-mode", "NX_PARTITIONING_MODE" );

	cfg.registerConfigOption("partitioning-priorityqueue", NEW Config::FlagOption( _usePriorities ), "Priority queue used as ready task queue (default: false)");
	cfg.registerArgOption("partitioning-priorityqueue", "schedule-priority");

	cfg.registerConfigOption("partitioning-movepages", NEW Config::FlagOption( _movePages ), "Move pages when mode=window? (default: false)");
	cfg.registerArgOption("partitioning-movepages", "partitioning-movepages");
	
	cfg.registerConfigOption( "partitioning-old-deps", NEW Config::FlagOption( _oldDep ), "Include immediately previous dependency (default: false)" );
	cfg.registerArgOption("partitioning-old-deps", "partitioning-old-deps");

	cfg.registerConfigOption( "partitioning-size", NEW Config::PositiveVar( _wSize ), "Size of the subgraph to partition (default: 128)" );
	cfg.registerArgOption( "partitioning-size", "partitioning-size" );

	cfg.registerConfigOption( "partitioning-window-intersection", NEW Config::PositiveVar( _wIntersect ), "Intersection with the next subgraph to partition (default: 64)" );
	cfg.registerArgOption( "partitioning-window-intersection", "partitioning-window-intersection" );

	cfg.registerConfigOption( "partitioning-window-initial-extra", NEW Config::IntegerVar( _wExtra ), "Number of extra tasks for the initial window (default: 0)" );
	cfg.registerArgOption( "partitioning-window-initial-extra", "partitioning-window-initial-extra" );

	cfg.registerConfigOption( "partitioning-steal", NEW Config::FlagOption( _steal ), "Enable the stealing for tasks not in the initial window (default: false)" );
	cfg.registerArgOption( "partitioning-steal", "partitioning-steal" );

	cfg.registerConfigOption( "partitioning-scotch-seed", NEW Config::IntegerVar( _seedSCOTCH ), "Seed for the SCOTCH partitioner" );
	cfg.registerArgOption( "partitioning-scotch-seed", "partitioning-scotch-seed" );

	cfg.registerConfigOption( "partitioning-internal-seed", NEW Config::IntegerVar( _seedPRNG ), "Seed for the rest of the scheduling" );
	cfg.registerArgOption( "partitioning-internal-seed", "partitioning-internal-seed" );

	cfg.registerConfigOption( "partitioning-unbalance", NEW Config::VarOption<double, Config::HelpFormat> ( _unbalance ), "Maximum unbalance ratio for initial window (default: 0.001" );
	cfg.registerArgOption( "partitioning-unbalance", "partitioning-unbalance" );

	
#ifdef NANOS_DEBUG_ENABLED
	cfg.registerConfigOption( "partitioning-check-graph", NEW Config::FlagOption( _checkGraph ), "Check the graph after building it (default: false)" );
	cfg.registerArgOption( "partitioning-check-graph", "partitioning-check-graph" );
#endif

	cfg.registerConfigOption( "partitioning-pause-threads", NEW Config::FlagOption( _pauseThreads ), "Do the initial metapart with threads paused (default: false)" );
	cfg.registerArgOption( "partitioning-pause-threads", "partitioning-pause-threads" );

	cfg.registerConfigOption( "partitioning-arch", NEW Config::StringVar( _archFilename ), "Filename for the architecture description in SCOTCH format (optional; disables autodiscovery of the architecture)" );
	cfg.registerArgOption( "partitioning-arch", "partitioning-arch" );
	cfg.registerEnvOption( "partitioning-arch", "NX_PARTITIONING_ARCH" );

	cfg.registerConfigOption( "partitioning-part-map", NEW Config::StringVar ( _partMapFilename ), "Map file between the parts and the sockets, needed when the architecture is more detailed than just the sockets" );
	cfg.registerArgOption( "partitioning-part-map", "partitioning-part-map" );
	cfg.registerEnvOption( "partitioning-part-map", "NX_PARTITIONING_PART_MAP" );

	cfg.registerConfigOption( "partitioning-distance-mat", NEW Config::StringVar ( _distanceMatFilename ), "Matrix of distances (optional)" );
	cfg.registerArgOption( "partitioning-distance-mat", "partitioning-distance-mat" );
	cfg.registerEnvOption( "partitioning-distance-mat", "NX_PARTITIONING_DISTANCE_MAT" );

#ifdef NANOS_DEBUG_ENABLED
	cfg.registerConfigOption( "partitioning-save-initial-graph", NEW Config::StringVar( _graphFilename ), "Filename for the for where to save the initial graph" );
	cfg.registerArgOption( "partitioning-save-initial-graph", "partitioning-save-initial-graph" );
#endif
      }
      
      virtual void init()
      {
	if (not metapartModes.valid(_mode)) {
	  fatal0( "The chosen metapart mode " << _mode << " is not valid!" );
	}

	if (_usePriorities)
	  sys.setDefaultSchedulePolicy( NEW MetapartSchedPolicy<WDPriorityQueue<> >( metapartModes.value(_mode), _oldDep, _wSize, _wIntersect, _wExtra, _steal, _movePages, _seedSCOTCH, _seedPRNG, _unbalance, _checkGraph, _pauseThreads, _archFilename, _partMapFilename, _distanceMatFilename, _graphFilename ) );

	sys.setDefaultSchedulePolicy( NEW MetapartSchedPolicy<WDDeque>( metapartModes.value(_mode), _oldDep, _wSize, _wIntersect, _wExtra, _steal, _movePages, _seedSCOTCH, _seedPRNG, _unbalance, _checkGraph, _pauseThreads, _archFilename, _partMapFilename, _distanceMatFilename, _graphFilename ) );
      }
    };
  }
}

DECLARE_PLUGIN("sched-metapart", nanos::ext::MetapartSchedPlugin);
