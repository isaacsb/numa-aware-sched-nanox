#include "schedule.hpp"
#include "wddeque.hpp"
#include "hashmap.hpp"
#include "plugin.hpp"
#include "system.hpp"
#include "config.hpp"
#include "os.hpp"

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
      locsched::DepMethod _dep;
      std::tr1::mt19937 _rand;
      
    public:
      DepSchedPolicy(bool steal, size_t minDepSize)
	: SchedulePolicy( "Dependency" ),
          _steal(steal),
	  _rand(time(NULL))
      {
	_dep.setRandObject(_rand);
        _dep.minDepSize(minDepSize);
      }

      virtual ~DepSchedPolicy()
      {}

    public:

      virtual void queue ( BaseThread *thread, WD &wd )
      {
	int socket;

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
	  _dep.updateSocket(&wd, socket, false);
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

        TeamData & tdata = (TeamData &) *thread->getTeam()->getScheduleData();
	
	WorkDescriptor * wd  = tdata.readyQueues[vNode].pop_front( thread );
	
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

	if (wd != NULL and vNode < _dep.numSockets())
	  _dep.updateSocket( wd, vNode, true );

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
	
	~TeamData() {
	  delete[] readyQueues;
	}
      };
      
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
            _dep.init();
          }
        }
        
	return tdata;
      }

      virtual ScheduleThreadData * createThreadData() { return NULL; }

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
      bool _priority;
      int _minDepSize;
      
    public:
      DepSchedPlugin()
	: Plugin( "Dependency scheduling Plugin", 1),
	  _steal(false),
	  _priority(false),
	  _minDepSize(1)
      {}
      
      
      virtual void config( Config &cfg )
      {
	cfg.setOptionsSection("Dependency scheduling", "Dependency scheduling module");
	
	cfg.registerConfigOption( "dep-steal", NEW Config::FlagOption( _steal ), "Enable stealing from other sockets (default: false)" );
	cfg.registerArgOption( "dep-steal", "dep-steal" );

	cfg.registerConfigOption("dep-priority", NEW Config::FlagOption( _priority ), "Use priority queue for the ready queues (default: false)");
	cfg.registerArgOption("dep-priority", "dep-priority");
	cfg.registerAlias("dep-priority", "schedule-priority", "Same as --dep-priority");
	cfg.registerArgOption("schedule-priority", "schedule-priority");

	cfg.registerConfigOption("dep-min-size", NEW Config::PositiveVar( _minDepSize ), "Minimum size (in bytes) of dependency to be considered (default: 1)");
	cfg.registerArgOption("dep-min-size", "dep-min-size");
        
      }
      
      virtual void init()
      {
	if (_priority)
	  sys.setDefaultSchedulePolicy( NEW DepSchedPolicy<WDPriorityQueue<> >( _steal, _minDepSize ) );
	else
	  sys.setDefaultSchedulePolicy( NEW DepSchedPolicy<WDDeque>( _steal, _minDepSize ) );
      }
    };
  }
}

DECLARE_PLUGIN("sched-dep", nanos::ext::DepSchedPlugin);
