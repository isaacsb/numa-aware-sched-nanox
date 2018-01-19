#include "schedule.hpp"
#include "wddeque.hpp"
#include "hashmap.hpp"
#include "plugin.hpp"
#include "system.hpp"
#include "config.hpp"

#define LOCALITY_USE_NUMA
#include "dep-method.hpp"

#include <fstream>
#include <tr1/random>


namespace nanos {
  namespace ext {

#define NANOS_SCHED_PRE_RAISE_EVENT(x)   NANOS_INSTRUMENT( \
      sys.getInstrumentation()->raiseOpenBurstEvent ( sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey( "precalculated" ), (x) ); )

#define NANOS_SCHED_PRE_CLOSE_EVENT       NANOS_INSTRUMENT( \
      sys.getInstrumentation()->raiseCloseBurstEvent ( sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey( "precalculated" ), 0 ); )

    template <class _WDQueue>
    class PrecalculatedMPSchedPolicy : public SchedulePolicy {
    private:
      int _numSockets;
      _WDQueue *_readyQueues;
      std::string _mapFile;
      int *_socketOrder;
      int _numTasks;
      int _numThreads;
      int _firstId;
      bool _steal;
      std::tr1::mt19937 _rand;

      locsched::DepMethod _dep;
      
    public:
      using SchedulePolicy::queue;
 
      PrecalculatedMPSchedPolicy( std::string mapFilePath, int numSockets, bool steal)
	: SchedulePolicy( "Predefined" ),
	  _mapFile( mapFilePath ),
	  _socketOrder( NULL ),
	  _numThreads( 0 ),
	  _firstId( -1 ),
	  _steal(steal),
	  _rand(time(NULL))
      {
        _dep.setRandObject(_rand);
      }

    private:
      void _readMap()
      {
	std::ifstream mapFile;
	mapFile.open(_mapFile.c_str());
	if ( not mapFile.good() ) {
	  fatal0( "Precalculated map scheduler: The map file is not valid" );
	}

	int mapSockets;
	mapFile >> mapSockets;

	if ( _numSockets < mapSockets ) {
	  fatal0( "Precalculated map scheduler: The number of sockets of the map is greater than that of the system" );
	}
	else if ( _numSockets > mapSockets ) {
	  warning0( "Precalculated map scheduler: The schedule is done for less sockets (" << mapSockets << ") than available (" << _numSockets << ")" );
	}

	mapFile >> _numTasks;

	_socketOrder = NEW int[_numTasks];

        int taskId, socketId;
	int i = 0;
        while (mapFile >> taskId >> socketId) {
          std::string aux;
          std::getline(mapFile, aux); // cleanup line
          _socketOrder[i] = socketId;
          ++i;
        }

	mapFile.close();
	
	if ( i != _numTasks ) {
	  fatal0( "Precalculated map scheduler: The number of tasks in the file does not match that of the header: " << i << " vs " << _numTasks );
	}
      }

    public:
      virtual ~PrecalculatedMPSchedPolicy()
      {
	delete[] _socketOrder;
      }

      virtual void queue ( BaseThread *thread, WD &wd )
      {
	// NANOS_SCHED_PRE_RAISE_EVENT( 1 );
	int socket;

	unsigned node = thread->runningOn()->getMyNodeNumber();
	int vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );

        int depth = wd.getDepth();
        if (sys.getPMInterface().getInterface() == PMInterface::OpenMP and depth >= 1) {
          --depth;
        }

        switch( depth ) {
        case 0: // Hideous implicit tasks...
          socket = _numSockets;
          break;
        default:
          int taskId = wd.getId() - _firstId;
          if (taskId >= _numTasks) {
            warning0( "Trying to schedule more tasks than map file has: " << taskId << " vs " << _numTasks );
            fatal0( "Execution aborted due to trying to schedule more tasks than map file has" );
          }
          else if (taskId >= 0) {
            socket = _socketOrder[taskId];
          }
          else {
            //socket = sys.getVirtualNUMANode( thread->runningOn()->getNUMANode() );
            socket = vNode;
          }
        }

	_readyQueues[socket].push_back( &wd );
	// NANOS_SCHED_PRE_CLOSE_EVENT;
      }

      virtual WD *atSubmit( BaseThread *thread, WD &newWD )
      {
	queue( thread, newWD );
	return 0;
      }

      virtual WD *atIdle( BaseThread *thread, int numSteals )
      {
	// NANOS_SCHED_PRE_RAISE_EVENT( 2 );
	unsigned node = thread->runningOn()->getNumaNode();
	int vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );
	//int vNode = thread->getId()%_numSockets;
	
	WorkDescriptor *wd = _readyQueues[vNode].pop_front( thread );

	if (wd == NULL) {
	  if (_steal and numSteals > 0) {
	    vNode = _rand()%_numSockets;
	    wd = _readyQueues[vNode].pop_front( thread );
	  }

	  if (wd == NULL)
	    wd = _readyQueues[_numSockets].pop_front( thread );
	}

        if (wd != NULL) {
          _dep.forceSocket(wd, vNode, node);
        }

	// NANOS_SCHED_PRE_CLOSE_EVENT;
	return wd;
      }

    private:
      class TeamData : public ScheduleTeamData
      {
      public:
	_WDQueue *_readyQueues;
	TeamData(int numSockets)
	{
	  _readyQueues = NEW _WDQueue[numSockets + 1];
	}
	~TeamData()
	{
	  delete[] _readyQueues;
	}
      };

    public:
      virtual size_t getTeamDataSize () const { return sizeof(TeamData); }
      virtual size_t getThreadDataSize () const { return 0; }

      virtual ScheduleTeamData * createTeamData ()
      {
        _numThreads = sys.getNumThreads();
        if (sys.getPMInterface().getInterface() == PMInterface::OpenMP)
          _firstId = _numThreads*2 + 2;
        else
          _firstId = _numThreads + 2;
	_numSockets = sys.getNumNumaNodes();
	TeamData * tdata = NEW TeamData(_numSockets);
	_readyQueues = tdata->_readyQueues;

        if (not _dep.initialized()) {
          SyncLockBlock depLock(_dep.getLock());
          if (not _dep.initialized()) {
            _dep.numSockets(_numSockets);
            _dep.init();
          }
        }
	
	_readMap();

	return tdata;
      }
      virtual ScheduleThreadData * createThreadData () { return 0; }
    };

    class PrecalculatedMPSchedPlugin : public Plugin {
    private:
      std::string _taskSocketMapFile;
      int _numSockets;
      bool _steal;
      bool _prio;

      void loadDefaultValues()
      {
	_numSockets = sys.getNumNumaNodes();
      }

    public:
      PrecalculatedMPSchedPlugin()
	: Plugin( "Predefined MP scheduling Plugin", 1),
	  _taskSocketMapFile(""),
	  _steal(false),
	  _prio(false)
      {}


      virtual void config( Config &cfg )
      {
	loadDefaultValues();
	cfg.setOptionsSection("Precalculated MP scheduling", "Precalculated map scheduling module with page movement");
	cfg.registerConfigOption( "precalculated-task-map-file", NEW Config::StringVar( _taskSocketMapFile ), "File where to read the map between tasks and numa nodes" );
	cfg.registerArgOption( "precalculated-task-map-file", "precalculated-task-map-file" );
	cfg.registerEnvOption( "precalculated-task-map-file", "NX_PRECALCULATED_MAP_FILE" );

        cfg.registerConfigOption( "precalculated-steal", NEW Config::FlagOption( _steal ), "Enable stealing from other sockets (default: false)" );
	cfg.registerArgOption( "precalculated-steal", "precalculated-steal" );

	cfg.registerConfigOption( "precalculated-priority", NEW Config::FlagOption( _prio ), "Take priority into account (default: false)" );
	cfg.registerArgOption( "precalculated-priority", "precalculated-priority" );
      }

      virtual void init()
      {
	// _taskSocketMapFile = "prueba";
	if (_prio)
	  sys.setDefaultSchedulePolicy( NEW PrecalculatedMPSchedPolicy<WDPriorityQueue<> >( _taskSocketMapFile, _numSockets, _steal ) );
	else
	  sys.setDefaultSchedulePolicy( NEW PrecalculatedMPSchedPolicy<WDDeque>( _taskSocketMapFile, _numSockets, _steal ) );
      }
    };
  }
}

#undef LOCALITY_USE_NUMA

DECLARE_PLUGIN("sched-precalculated_mp", nanos::ext::PrecalculatedMPSchedPlugin);
