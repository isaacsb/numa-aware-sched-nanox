#include "schedule.hpp"
#include "wddeque.hpp"
#include "hashmap.hpp"
#include "plugin.hpp"
#include "system.hpp"
#include "config.hpp"
#include "os.hpp"

#define LOCALITY_USE_NUMA
#include "rip-method.hpp"
#include "dep-method.hpp"

#include <map>
#include <vector>
#include <cassert>
#include <sstream>

#include <utility>
#include <cmath>
#include <tr1/random>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h> // interposition...

#ifndef RIP_SUFFIX
#define RIP_SUFFIX nosuffix
#endif

#define MY_STRINGIZER_INNER(x) #x
#define MY_STRINGIZER(x) MY_STRINGIZER_INNER(x)
#define STR_RIP_SUFFIX MY_STRINGIZER(RIP_SUFFIX)
#define MY_ADD_SUFFIX_INNER(x, s) x ## _ ## s
#define MY_ADD_SUFFIX(x, s) MY_ADD_SUFFIX_INNER(x, s)
#define ADD_SUFFIX(x) MY_ADD_SUFFIX(x, RIP_SUFFIX)

namespace nanos {
  namespace ext {
    class RipModes {
    public:
      enum _RipModes {
        ERROR=-1,
	WINDOW=0,
	DEPENDENCY
      };
      typedef _RipModes type;
    private:
      std::map<std::string, _RipModes> _allowed;
    public:
      RipModes() {
        _allowed["dep"] = DEPENDENCY;
        _allowed["mw"] = WINDOW;
      }
      bool valid(const std::string & s, const char *) { return _allowed.find(s) != _allowed.end(); }
      _RipModes value(const std::string & s) { return _allowed.find(s)->second; }
      std::string defaultMode() { return "dep"; }

      std::string validList()
      {
	std::ostringstream aux;
	std::map<std::string, _RipModes>::const_iterator it = _allowed.begin();

	if (it != _allowed.end()) {
	  aux << it->first;
	  for (++it; it != _allowed.end(); ++it) {
	    aux << ", " << it->first;
	  }
	}

	return aux.str();
      }
    };
    
    template <typename _WDQueue>
    class RipSchedPolicy : public SchedulePolicy {
    public:
      typedef RipSchedPolicy<_WDQueue> LocalPolicy;
    private:
      bool _steal;
      bool _movePages;
      RipModes::type _mode;
      int _wSize, _wIntersection, _wInitExtra;
      double _imbalance;
      std::string _distanceMatFilename;
      std::tr1::mt19937 _rand;
      
      locsched::DepMethod _dep;
      locsched::RipMethod _rip;

      std::list<WD *> _tmpQueue;
      Lock _tmpQueueLock;

      Atomic<int> _lastPartitionScheduled;

      bool _manual;
      std::list<int> _wdStops;

      Atomic<bool> _blocked;
      
    public:
      RipSchedPolicy(bool steal, bool movePages, size_t minDepSize, RipModes::type mode, int wSize, int wIntersection, int wInitExtra, double imbalance, const std::string & distanceMatFilename, int seed, int partitionerSeed, const std::string & endOfLoopName)
	: SchedulePolicy( "Dependency" ),
          _steal(steal),
          _movePages(movePages),
          _mode(mode),
          _wSize( mode == RipModes::DEPENDENCY ? wSize : wSize + wInitExtra),
          _wIntersection(wIntersection),
          _wInitExtra(wInitExtra),
          _imbalance(imbalance),
          _distanceMatFilename(distanceMatFilename),
	  _rand(seed),
          _lastPartitionScheduled(0),
          _manual(false),
          _blocked(false)
      {
	_dep.setRandObject(_rand);
        _dep.minDepSize(minDepSize);
        _rip.minDepSize(minDepSize);
        _rip.seedPartitioner(partitionerSeed);

        if (endOfLoopName != "") {
          void (**helper_func)() = (void (**)()) dlsym(RTLD_DEFAULT, endOfLoopName.c_str());
          if (helper_func == NULL) {
            fatal0("Could not find \"end of loop\" function with name "
                   << endOfLoopName << "! Stopping...");
          }
          else if (*helper_func != NULL) {
            fatal0("The \"end of loop\" function with name "
                   << endOfLoopName << "is already set! Stopping...");
          }
          else {
            *helper_func = &LocalPolicy::endOfLoop;
            _manual = true;
            _wdStops.push_back(0);
          }
        }
      }

      virtual ~RipSchedPolicy()
      {}

    public:

      static void partitionDone(SchedulePolicy * sp)
      {
        static_cast<RipSchedPolicy<_WDQueue> *>(sp)->_partitionDone();
      }

      virtual void queue(BaseThread *thread, WD &wd)
      {
	int socket = _dep.numSockets() + 1;
        if (not wd.isRuntimeTask()) {
          int depth = wd.getDepth();
          if (sys.getPMInterface().getInterface() == PMInterface::OpenMP and depth >= 1) {
            --depth;
          }
          
          switch (depth) {
          case 0: // Hideous implicit tasks...
            socket = _dep.numSockets();
            break;
          case 1:
          default:
            socket = _rip.chooseSocket(&wd);
            
            if (socket == -2 and _mode == RipModes::DEPENDENCY) {
              socket = _dep.chooseSocket(&wd);
              _dep.updateSocket(&wd, socket, false);
            }
            break;
          }
        }

        if (socket < 0) {
          SyncLockBlock queueLock(_tmpQueueLock);
          socket = _rip.chooseSocket(&wd);
          if (socket < 0) {
            WDData::get(wd).tdata = (TeamData*) thread->getTeam()->getScheduleData();
            _tmpQueue.push_back(&wd);
            return;
          }
        }

	TeamData & tdata = * (TeamData *) thread->getTeam()->getScheduleData();
        tdata.readyQueues[socket].push_back( &wd );
      }

      virtual void queue ( BaseThread ** threads, WD ** wds, size_t numElems )
      {
        SchedulePolicy::queue(threads, wds, numElems);
      }

      virtual WD * atSubmit( BaseThread *thread, WD &newWD )
      {
	queue( thread, newWD );

	return 0;
      }

      virtual WD * atIdle( BaseThread *thread, int numSteals )
      {
        int vNode = _dep.numSockets() + 1;
	unsigned node = thread->runningOn()->getNumaNode();
        TeamData & tdata = (TeamData &) *thread->getTeam()->getScheduleData();
	WorkDescriptor * wd  = tdata.readyQueues[vNode].pop_front( thread );
	
	if (wd == NULL) {
          vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );
          wd = tdata.readyQueues[vNode].pop_front( thread );
          
	  if (wd == NULL and _steal and numSteals > 0) {
            std::tr1::uniform_int<int> gen;
	    vNode = gen(_rand, _dep.numSockets());
	    wd = tdata.readyQueues[vNode].pop_front( thread );
	  }
	  
	  if (wd == NULL) {
	    vNode = _dep.numSockets();
	    wd = tdata.readyQueues[vNode].pop_front( thread );
	  }
	}

	if (wd != NULL and vNode < _dep.numSockets()) {
          if (_mode == RipModes::WINDOW)
            _rip.updateSocket(wd, vNode, true);

          if (_movePages)
            _dep.forceSocket(wd, vNode, node);
          else if (_mode == RipModes::DEPENDENCY)
            _dep.updateSocket(wd, vNode, true);
        }

	return wd;
      }

      virtual WD * atPrefetch ( BaseThread *thread, WD &current )
      {
        return SchedulePolicy::atPrefetch(thread, current);
      }
        
      virtual WD * atBeforeExit ( BaseThread *thread, WD &current, bool schedule )
      {
        return SchedulePolicy::atBeforeExit(thread, current, schedule);
      }

      virtual void atCreate(DependableObject & depObj)
      {
        DOSubmit *dp = (DOSubmit *) &depObj;
	if (dp == NULL) {
	  return;
	}

	WD *wd = (WD *) dp->getRelatedObject();
        int depth = wd->getDepth();
        if (sys.getPMInterface().getInterface() == PMInterface::OpenMP and depth >= 1) {
          --depth;
        }
	if (depth == 0 or wd->isRuntimeTask() or
            (_lastPartitionScheduled > 0 and _mode == RipModes::DEPENDENCY)) {
          return;
        }

        int current = _rip.addTask(*wd);

        if (_manual)
          return;
        
        int first = std::max(_lastPartitionScheduled - _wIntersection + 1, 1);
        if (_lastPartitionScheduled == 0) {
            first = 1;
        }

        int last = first + _wSize - 1;
        if (current == last) {
          if (_lastPartitionScheduled == 0) {
            _wSize -= _wInitExtra;
          }
          
          _schedulePartition(first, last, last - _wIntersection + 1);
        }
      }

      virtual WD * atBlock(BaseThread * thread, WD * current)
      {
        _blocked = true;
        int first = std::max(_lastPartitionScheduled - _wIntersection + 1, 1);
        int last = _rip.lastAddedTask();
        if (last > _lastPartitionScheduled) {
          if (_manual)
            _endOfLoop(true);
          else {
            if (_lastPartitionScheduled == 0) {
              first = 1;
              _wSize -= _wInitExtra;
            }
            _schedulePartition(first, last, last - _wIntersection + 1);
          }
        }

        return SchedulePolicy::atBlock(thread, current);
      }

      virtual bool testDequeue()
      {
        return (sys.getReadyNum() > 0) or (_rip.lastAddedTask() > _lastPartitionScheduled);
      }

      static void endOfLoop()
      {
        ((LocalPolicy*) sys.getDefaultSchedulePolicy())->_endOfLoop();
      }

    private:

      void _endOfLoop(bool force=false)
      {
        if (_lastPartitionScheduled.value() == this->_rip.lastAddedTask()) {
          // warning("WOOOPS, already added, forced? " << force);
          // SyncLockBlock b(_tmpQueueLock);
          // warning("tmp queue size: " << _tmpQueue.size());
          // warning("tmp queue elements: ");
          // for (std::list<WD *>::const_iterator it = _tmpQueue.begin();
          //      it != _tmpQueue.end();
          //      ++it) {
          //   warning("\t" << WDData::getRipInfo(**it).id);
          // }
        }
        else {
          _wdStops.push_back(this->_rip.lastAddedTask());
          if (_wdStops.size() == size_t(_wSize + 1) or force) {
            int first = _wdStops.front() + 1;
            _wdStops.pop_front();
            int last = _wdStops.back();
            while (_wdStops.size() > size_t(_wIntersection + 1))
              _wdStops.pop_front();
          
            this->_schedulePartition(first, last, _wdStops.front() + 1);
          }
        }
      }

      struct _PartitionerArgs
      {
        locsched::RipMethod * rip;
        int first, last, next;
      };

      void _schedulePartition(int first, int last, int next)
      {
        _lastPartitionScheduled = last;

        nanos_smp_args_t transformer = { (void (*)(void *))(void (*)(_PartitionerArgs &)) _callPartitioner };
        nanos_device_t dev = {_ripSmpFactory, &transformer};
        _PartitionerArgs * args = NULL;
        WD * ripWd = NULL;
        sys.createWD(&ripWd, 1, &dev, sizeof(_PartitionerArgs &), __alignof__(_PartitionerArgs), (void **) &args, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, "@@partitioner", NULL);

        if (ripWd == NULL) {
          fatal0( "Could not create WorkDescriptor for graph partitioning. Exiting..." );
        }
        
        ripWd->setRuntimeTask(true);
        ::nanos_region_dimension_t dimension;
        dimension.size = sizeof(locsched::RipMethod);
        dimension.lower_bound = 0L;
        dimension.accessed_length = 1L * sizeof(locsched::RipMethod);

        DataAccess deps(&_rip, true, true, false, false, false, 1, &dimension, 0L);

        args->rip = &_rip;
        args->first = first;
        args->last = last;
        args->next = next;

        sys.submitWithDependencies(*ripWd, 1, &deps);
      }

      static void _callPartitioner(_PartitionerArgs & args)
      {
        args.rip->partition(args.first, args.last, args.next);
      }

      static void * _ripSmpFactory( void *argTransform )
      {
	nanos_smp_args_t *smp = reinterpret_cast<nanos_smp_args_t *>(argTransform);
	return ( void * )new ext::SMPDD( smp ? smp->outline : NULL );
      }

      void _partitionDone()
      {
        SyncLockBlock queueLock(_tmpQueueLock);
        _rip.acquireChooseSocket();

        std::list<WD *>::iterator it = _tmpQueue.begin();
	while (it != _tmpQueue.end()) {
	  WD *wd = *it;
          int socket = _rip.chooseSocketUnprotected(wd);

	  if (socket < 0) {
            debug("Bad socket for " << wd->getId());
	    ++it;
	    continue;
	  }
	  
	  it = _tmpQueue.erase(it);
          if (_mode == RipModes::DEPENDENCY)
            _dep.updateSocket(wd, socket, false);
          
          WDData::get(*wd).tdata->readyQueues[socket].push_back(wd);
	}

        if (not _tmpQueue.empty()) {
          debug( "Queue is not empty!! " << _tmpQueue.size() );
        }
        _rip.releaseChooseSocket();
      }

      struct TeamData : public ScheduleTeamData
      {
	_WDQueue * readyQueues;
	
	TeamData(int sockets)
          : ScheduleTeamData()
	{
	  readyQueues = NEW _WDQueue[sockets + 2];
	}
	
	~TeamData() {
	  delete[] readyQueues;
	}
      };

      struct WDData : public ScheduleWDData
      {
        TeamData *tdata;
        locsched::RipMethod::RipInfo ripInfo;
	WDData(TeamData *tdata_=NULL)
	  : tdata(tdata_)
	{}

	~WDData()
	{}

	static WDData & get(WD & wd)
	{
	  return *static_cast<WDData*>(wd.getSchedulerData());
	}

        static locsched::RipMethod::RipInfo & getRipInfo(WD & wd)
        {
          return get(wd).ripInfo;
        }
      };

      void _initArch(std::vector<locsched::SocketInfo> & arch) {
        int numSockets = sys.getNumNumaNodes();
        arch.resize(numSockets);
        for (int i = 0; i < numSockets; ++i) {
          arch[i].distances.resize(numSockets, 0);
        }
        
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
                    arch[node_a].distances[node_b] = 1;
                  }
                  else if (node_b >= 0) {
                    // myDist[node_a][node_b] = distance;
                    arch[node_a].distances[node_b] = 1<<int(((distance+9)*2)/10);
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
                  arch[node_a].distances[node_b] = 1;
                }
                else if (node_a >= 0 and node_b >= 0) {
                  // myDist[node_a][node_b] = distance;
                  arch[node_a].distances[node_b] = 1<<int(((distance+9)*2)/10);
                }
              }
            }
          }
          else {
            fatal0("Could not read distances from the distance file");
          }
        }

        int numThreads = sys.getNumThreads();
        for (int i = 0; i < numThreads; ++i) {
          BaseThread *t = sys.getWorker(i);
          if (t != NULL) {
            int node = numanodes[t->runningOn()->getNumaNode()];
            ++arch[node].weight;
          }
          else {
            fatal0("Problem with thread " << i <<": it seems not to exist");
          }
        }
      }
      
    public:
      virtual size_t getTeamDataSize() const { return sizeof(TeamData); }
      virtual size_t getThreadDataSize() const { return 0; }

      virtual ScheduleTeamData * createTeamData () {
        int sockets = sys.getNumNumaNodes();
	ScheduleTeamData * tdata = NEW TeamData(sockets);

        if (not _dep.initialized()) {
          SyncLockBlock depLock(_dep.getLock());
          if (not _dep.initialized()) {
            _dep.numSockets(sockets);
            _dep.setRandObject(_rand);
            _dep.init();
          }
        }

        if (not _rip.initialized()) {
          SyncLockBlock ripLock(_rip.getLock());
          if (not _rip.initialized()) {
            std::vector<locsched::SocketInfo> arch;
            _initArch(arch);
            _rip.archInfo(arch);
            
            _rip.setRandObject(_rand);
            _rip.ripInfo(WDData::getRipInfo);
            _rip.parentSched(this);
            _rip.afterPartition(RipSchedPolicy::partitionDone);
            
            _rip.imbalance(_imbalance);
            
            _rip.init();
          }
        }
        
	return tdata;
      }

      virtual ScheduleThreadData * createThreadData() { return NULL; }

      virtual size_t getWDDataSize () const { return sizeof( WDData ); }
      virtual size_t getWDDataAlignment () const { return __alignof__( WDData ); }
      virtual void initWDData ( void * data ) const
      {
	NEW (data)WDData();
      }

      virtual bool usingPriorities() const;

      virtual std::string getSummary() const
      {
        std::ostringstream oss;
        oss << "RIP scheduler stats" << '\n'
            << "sockets: " << _rip.numSockets() << '\n'
            << "numa nodes: " << sys.getNumNumaNodes() << '\n';

        return oss.str();
      }
    };

    template<>
    inline bool RipSchedPolicy<WDPriorityQueue<> >::usingPriorities() const
    {
      return true;
    }

    template<>
    inline bool RipSchedPolicy<WDDeque>::usingPriorities() const
    {
      return false;
    }

    class ADD_SUFFIX(RipSchedPlugin) : public Plugin {
    private:
      bool _steal;
      bool _movePages;
      bool _priority;
      int _minDepSize;
      int _wSize;
      int _wIntersection;
      int _wInitExtra;
      double _imbalance;
      int _seed;
      int _partitionerSeed;
      std::string _mode;
      std::string _distanceMat;
      std::string _endOfLoop;
      RipModes _ripModes;
      
    public:
      ADD_SUFFIX(RipSchedPlugin)()
        : Plugin( "RIP scheduling Plugin" STR_RIP_SUFFIX, 1),
          _steal(false),
          _movePages(false),
          _priority(false),
          _minDepSize(1),
          _wSize(128),
          _wIntersection(0),
          _wInitExtra(0),
          _imbalance(0.01),
          _seed(time(NULL)),
          _partitionerSeed(time(NULL) + 1)
      {
        _mode = _ripModes.defaultMode();
      }
      
      
      virtual void config( Config &cfg )
      {
        cfg.setOptionsSection("RIP scheduling (" STR_RIP_SUFFIX ")", "RIP scheduling module (" STR_RIP_SUFFIX ")");

        cfg.registerConfigOption( "rip-mode", NEW Config::StringVar( _mode ), "Mode for partitioning the rest of the graph. Allowed values: " + _ripModes.validList() + " (default: " + _mode + ")" );
        cfg.registerArgOption( "rip-mode", "rip-mode" );
        cfg.registerEnvOption( "rip-mode", "NX_RIP_MODE" );
        
        cfg.registerConfigOption( "rip-steal", NEW Config::FlagOption( _steal ), "Enable stealing from other sockets (default: false)" );
        cfg.registerArgOption( "rip-steal", "rip-steal" );

        cfg.registerConfigOption( "rip-move-pages", new Config::FlagOption( _movePages ), "Enable page movement of output data (default: false)" );
        cfg.registerArgOption( "rip-move-pages", "rip-move-pages" );

        cfg.registerConfigOption("rip-priority", NEW Config::FlagOption( _priority ), "Use priority queue for the ready queues (default: false)");
        cfg.registerArgOption("rip-priority", "rip-priority");
        cfg.registerAlias("rip-priority", "schedule-priority", "Same as --rip-priority");
        cfg.registerArgOption("schedule-priority", "schedule-priority");

        cfg.registerConfigOption("dep-min-size", NEW Config::PositiveVar( _minDepSize ), "Minimum size (in bytes) of dependency to be considered (default: 1)");
        cfg.registerArgOption("dep-min-size", "dep-min-size");

        cfg.registerConfigOption( "rip-size", NEW Config::PositiveVar( _wSize ), "Size of the subgraph to partition (default: 128)" );
        cfg.registerArgOption( "rip-size", "rip-size" );

        cfg.registerConfigOption( "rip-window-intersection", NEW Config::PositiveVar( _wIntersection ), "Intersection with the next subgraph to partition (default: 64)" );
        cfg.registerArgOption( "rip-window-intersection", "rip-window-intersection" );

        cfg.registerConfigOption( "rip-window-initial-extra", NEW Config::IntegerVar( _wInitExtra ), "Number of extra tasks for the initial window (default: 0)" );
        cfg.registerArgOption( "rip-window-initial-extra", "rip-window-initial-extra" );

        cfg.registerConfigOption( "rip-imbalance", NEW Config::VarOption<double, Config::HelpFormat> ( _imbalance ), "Maximum imbalance ratio for initial window (default: 0.01" );
        cfg.registerArgOption( "rip-imbalance", "rip-imbalance" );

        cfg.registerConfigOption( "rip-partitioner-seed", NEW Config::IntegerVar( _partitionerSeed ), "Seed for the partitioner" );
	cfg.registerArgOption( "rip-partitioner-seed", "rip-partitioner-seed" );

	cfg.registerConfigOption( "rip-internal-seed", NEW Config::IntegerVar( _seed ), "Internal random seed for the scheduler" );
	cfg.registerArgOption( "rip-internal-seed", "rip-internal-seed" );

        cfg.registerConfigOption( "rip-distance-mat", NEW Config::StringVar ( _distanceMat ), "Matrix of distances (optional)" );
        cfg.registerArgOption( "rip-distance-mat", "rip-distance-mat" );

        cfg.registerConfigOption( "rip-endofloop-name", NEW Config::StringVar ( _endOfLoop ), "Global name of pointer to function returning void (optional; address pointed to must be NULL)" );
        cfg.registerArgOption( "rip-endofloop-name", "rip-endofloop-name" );
      }
      
      virtual void init()
      {
        if (_priority)
          sys.setDefaultSchedulePolicy( NEW RipSchedPolicy<WDPriorityQueue<> >( _steal, _movePages, _minDepSize, _ripModes.value(_mode), _wSize, _wIntersection, _wInitExtra, _imbalance, _distanceMat, _seed, _partitionerSeed, _endOfLoop ) );
        else
          sys.setDefaultSchedulePolicy( NEW RipSchedPolicy<WDDeque>( _steal, _movePages, _minDepSize, _ripModes.value(_mode), _wSize, _wIntersection, _wInitExtra, _imbalance, _distanceMat, _seed, _partitionerSeed, _endOfLoop ) );
      }
    };
  }
}

DECLARE_PLUGIN("sched-rip" STR_RIP_SUFFIX, nanos::ext::ADD_SUFFIX(RipSchedPlugin));
