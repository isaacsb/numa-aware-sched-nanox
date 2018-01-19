#include "schedule.hpp"
#include "wddeque.hpp"
#include "hashmap.hpp"
#include "plugin.hpp"
#include "system.hpp"
#include "config.hpp"
#include "os.hpp"

#define LOCALITY_USE_NUMA
#include "dep-method.hpp"

#include <map>
#include <vector>
#include <cassert>

#include <utility>
#include <cmath>
#include <tr1/random>

namespace nanos {
  namespace ext {
    template <typename _WDQueue>
    class DepSchedPolicy : public SchedulePolicy {
    private:
      bool _steal;
      bool _movePages;
      locsched::DepMethod _dep;
      std::tr1::mt19937 _rand;
      
    public:
      DepSchedPolicy(bool steal, bool movePages, locsched::DepMethod::NextMode nextMode, size_t minDepSize)
	: SchedulePolicy( "Dependency" ),
          _steal(steal),
          _movePages(movePages),
          _dep(nextMode),
	  _rand(time(NULL))
      {
        if (movePages) {
          warning0("Page migration activated!");
        }
	_dep.setRandObject(_rand);
        _dep.minDepSize(minDepSize);
      }

      virtual ~DepSchedPolicy()
      {}

    public:

      virtual void queue ( BaseThread *thread, WD &wd )
      {
	int socket;
        bool done = false;
        int depth = wd.getDepth();

        if (sys.getPMInterface().getInterface() == PMInterface::OpenMP and depth >= 1) {
          --depth;
        }

	switch( depth ) {
	case 0: // Hideous implicit tasks...
	  socket = _dep.numSockets();
	  break;
	case 1:
	default:
	  socket = _dep.chooseSocket(&wd);
          ThreadData *thdata = (ThreadData *) thread->getTeamData()->getScheduleData();
          if (socket < 0) {
            int thid = -socket - 1;
            BaseThread &dest = thread->getTeam()->getThread(thid);
            unsigned node = dest.runningOn()->getNumaNode();
            socket = static_cast<int>( sys.getVirtualNUMANode( node ) );
            if ( dest.getTeam() != NULL )
              thdata = (ThreadData *) dest.getTeamData()->getScheduleData();
            done = true;
            WDData::getDepInfo(wd).reset();
          }
	  _dep.updateSocket(&wd, socket, false);

          if (done) {
            thdata->_readyQueue.push_back ( &wd );
            return;
          }
	  
	  break;
	}

	TeamData & tdata = (TeamData &) *thread->getTeam()->getScheduleData();
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
	unsigned node = thread->runningOn()->getNumaNode();
	int vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );

        ThreadData & thdata = (ThreadData &) *thread->getTeamData()->getScheduleData();

        WorkDescriptor * wd  = thdata._readyQueue.pop_front( thread );
        
        TeamData & tdata = (TeamData &) *thread->getTeam()->getScheduleData();
        if (wd == NULL)
          wd = tdata.readyQueues[vNode].pop_front( thread );
	
	if (wd == NULL) {
	  if (_steal and numSteals > 0) {
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
          if (_movePages)
            _dep.forceSocket(wd, vNode, node);
          else
            _dep.updateSocket( wd, vNode, true );
        }

	return wd;
      }

      WD * atPrefetch ( BaseThread *thread, WD &current )
      {
        return SchedulePolicy::atPrefetch(thread, current);
      }
        
      WD * atBeforeExit ( BaseThread *thread, WD &current, bool schedule )
      {
        return SchedulePolicy::atBeforeExit(thread, current, schedule);
      }

    private:

      struct TeamData : public ScheduleTeamData
      {
	_WDQueue * readyQueues;
	
	TeamData(int sockets)
          : ScheduleTeamData()
	{
	  readyQueues = NEW _WDQueue[sockets + 1];
	}
	
	virtual ~TeamData() {
	  delete[] readyQueues;
	}
      };

      struct ThreadData : public ScheduleThreadData
      {
        /*! queue of ready tasks to be executed */
        _WDQueue _readyQueue;

        ThreadData () : ScheduleThreadData()
        {}
        virtual ~ThreadData () {}
      };

      struct WDData : public ScheduleWDData
      {
        locsched::DepMethod::DepInfo depInfo;
	WDData() {}

	virtual ~WDData() {}

	static WDData & get(WD & wd)
	{
	  return *static_cast<WDData*>(wd.getSchedulerData());
	}

        static locsched::DepMethod::DepInfo & getDepInfo(WD & wd)
        {
          return get(wd).depInfo;
        }
      };
      
    public:
      virtual size_t getTeamDataSize() const { return sizeof(TeamData); }
      virtual size_t getThreadDataSize() const { return sizeof(ThreadData); }

      virtual ScheduleTeamData * createTeamData () {
        int sockets = sys.getNumNumaNodes();
	ScheduleTeamData * tdata = NEW TeamData(sockets);

        if (not _dep.initialized()) {
          SyncLockBlock depLock(_dep.getLock());
          if (not _dep.initialized()) {
            warning0( "Using " << sys.getNumThreads() << " threads and " << sockets << " sockets." );
            _dep.numThreads(sys.getNumThreads());
            _dep.numSockets(sockets);
            _dep.depInfo(WDData::getDepInfo);
            _dep.init();
          }
        }
        
	return tdata;
      }

      virtual ScheduleThreadData * createThreadData() { return NEW ThreadData(); }

      virtual size_t getWDDataSize () const { return sizeof( WDData ); }
      virtual size_t getWDDataAlignment () const { return __alignof__( WDData ); }
      virtual void initWDData ( void * data ) const { NEW (data)WDData(); }

      virtual bool usingPriorities() const;
    };

    template<>
    inline bool DepSchedPolicy<WDPriorityQueue<> >::usingPriorities() const
    {
      return true;
    }

    template<>
    inline bool DepSchedPolicy<WDDeque>::usingPriorities() const
    {
      return false;
    }

    class DepSchedPlugin : public Plugin {
    private:
      bool _steal;
      bool _movePages;
      bool _priority;
      int _minDepSize;
      bool _socketCyclic;
      bool _cpuCyclic;
      
    public:
      DepSchedPlugin()
	: Plugin( "Dependency scheduling Plugin", 1),
	  _steal(false),
          _movePages(false),
	  _priority(false),
	  _minDepSize(1),
          _socketCyclic(false),
          _cpuCyclic(false)
      {}
      
      
      virtual void config( Config &cfg )
      {
	cfg.setOptionsSection("Dependency scheduling", "Dependency scheduling module");
	
	cfg.registerConfigOption( "dep-steal", NEW Config::FlagOption( _steal ), "Enable stealing from other sockets (default: false)" );
	cfg.registerArgOption( "dep-steal", "dep-steal" );

        cfg.registerConfigOption( "dep-move-pages", new Config::FlagOption( _movePages ), "Enable page movement of output data (default: false)" );
        cfg.registerArgOption( "dep-move-pages", "dep-move-pages" );
        cfg.registerAlias("dep-move-pages", "rip-move-pages", "Same as --dep-move-pages");
        cfg.registerArgOption("rip-move-pages", "rip-move-pages");

	cfg.registerConfigOption("dep-priority", NEW Config::FlagOption( _priority ), "Use priority queue for the ready queues (default: false)");
	cfg.registerArgOption("dep-priority", "dep-priority");
	cfg.registerAlias("dep-priority", "schedule-priority", "Same as --dep-priority");
	cfg.registerArgOption("schedule-priority", "schedule-priority");

	cfg.registerConfigOption("dep-min-size", NEW Config::PositiveVar( _minDepSize ), "Minimum size (in bytes) of dependency to be considered (default: 1)");
	cfg.registerArgOption("dep-min-size", "dep-min-size");

	cfg.registerConfigOption("dep-socket-cyclic", NEW Config::FlagOption( _socketCyclic ), "Socket-cyclic instead of random (default: false)");
	cfg.registerArgOption("dep-socket-cyclic", "dep-socket-cyclic");

	cfg.registerConfigOption("dep-cpu-cyclic", NEW Config::FlagOption( _cpuCyclic ), "Cpu-cyclic instead of random (default: false)");
	cfg.registerArgOption("dep-cpu-cyclic", "dep-cpu-cyclic");
        
      }
      
      virtual void init()
      {
	if (_priority)
	  sys.setDefaultSchedulePolicy( NEW DepSchedPolicy<WDPriorityQueue<> >( _steal, _movePages, _socketCyclic ? locsched::DepMethod::SOCKET_CYCLIC : ( _cpuCyclic ? locsched::DepMethod::CPU_CYCLIC : locsched::DepMethod::RANDOM ), _minDepSize ) );
	else
	  sys.setDefaultSchedulePolicy( NEW DepSchedPolicy<WDDeque>( _steal, _movePages, _socketCyclic ? locsched::DepMethod::SOCKET_CYCLIC : ( _cpuCyclic ? locsched::DepMethod::CPU_CYCLIC : locsched::DepMethod::RANDOM ), _minDepSize ) );
      }
    };
  }
}

DECLARE_PLUGIN("sched-dep", nanos::ext::DepSchedPlugin);
