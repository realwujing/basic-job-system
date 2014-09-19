
#ifndef COMMONJOBSYSTEM_HEADER
#define COMMONJOBSYSTEM_HEADER

#pragma once

#include <vector>
#include <list>
#include <deque>
#include <array>
#include <thread>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

#ifdef WIN32
#	define WINDOWS
#endif

#ifdef WINDOWS
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#	undef min
#	undef max
#endif

#define JOBSYSTEM_ENABLE_PROFILING						/// Enables profiling visualization and basic stats, displayed on JobManager destruction.

namespace jobsystem
{
	size_t GetBit( size_t n ) { return 1 << n; }

	typedef std::function<void()> JobDelegate;			/// Structure of callbacks that can be requested as jobs.
	
	/**
	 * Global system components.
	 */
	std::atomic<size_t>				s_nextJobId = 0;	/// Job ID assignment for debugging / profiling.
	std::mutex						s_signalLock;		/// Global mutex for worker signaling.
	std::condition_variable			s_signalThreads;	/// Global condition var for worker signaling.
	std::atomic<size_t>				s_activeWorkers = 0;

	/**
	 * Offers access to the state of job.
	 * In particular, callers can use the Wait() function to ensure a given job is complete,
	 * or Cancel().
	 * Note, however, that doing so is not good practice with job systems. If no hardware threads
	 * are available to process a given job, you can stall the caller for significant time.
	 *
	 * Internally, the state manages dependencies as well as atomics describing the status of the job.
	 */
	typedef std::shared_ptr<class JobState> JobStatePtr;

	class JobState
	{
	private:

		friend class JobSystemWorker;
		friend class JobManager;

		std::atomic<bool>			m_done;				/// Has the job executed to completion?
		std::atomic<bool>			m_cancel;			/// Is the job pending cancellation?
		std::atomic<bool>			m_ready;			/// Has the job been marked as ready for processing?

		std::vector<JobStatePtr>	m_dependants;		/// List of dependent jobs.
		std::atomic<int>			m_dependencies;		/// Number of outstanding dependencies.

		size_t						m_jobId;			/// Debug/profiling ID.
		char						m_debugChar;		/// Debug character for profiling display.

		void SetActive()
		{
			m_done.store( false, std::memory_order_release );
		}

		void SetDone()
		{
			for( JobStatePtr dependant : m_dependants )
			{
				dependant->m_dependencies.fetch_add( -1, std::memory_order_relaxed );
			}

			m_done.store( true, std::memory_order_release );
		}

		bool AwaitingCancellation() const
		{
			return m_cancel.load( std::memory_order_relaxed );
		}

	public:

		JobState() : m_debugChar( 0 )
		{
			m_jobId = s_nextJobId++;

			m_ready.store( false, std::memory_order_release );
		}

		~JobState() {}

		void SetReady()
		{
			m_ready.store( true, std::memory_order_release );

			s_signalThreads.notify_all();
		}

		bool IsDone() const
		{
			return m_done.load( std::memory_order_acquire );
		}

		void Cancel()
		{
			m_cancel.store( true, std::memory_order_relaxed );
		}

		void Wait( size_t maxWaitMicroseconds = 0 )
		{
			size_t waitedMicroseconds = 0;

			enum { kWaitMicrosecondsPerIter = 10 };

			while( !IsDone() )
			{
				std::this_thread::sleep_for( std::chrono::microseconds( kWaitMicrosecondsPerIter ) );

				if( maxWaitMicroseconds != 0 )
				{
					waitedMicroseconds += kWaitMicrosecondsPerIter;

					if( waitedMicroseconds > maxWaitMicroseconds )
					{
						break;
					}

				}
			}
		}

		void AddDependant( JobStatePtr dependant )
		{
			m_dependants.push_back( dependant );

			dependant->m_dependencies.fetch_add( 1, std::memory_order_relaxed );
		}

		bool AreDependenciesMet() const
		{
			if( !m_ready.load( std::memory_order_acquire ) )
			{
				return false;
			}

			if( m_dependencies.load( std::memory_order_relaxed ) > 0 )
			{
				return false;
			}

			return true;
		}
	};

	/**
	 * Represents an entry in a job queue.
	 * - A delegate to invoke
	 * - Internal job state
	 */
	struct JobQueueEntry
	{
		JobDelegate		m_delegate;		/// Delegate to invoke for the job.
		JobStatePtr		m_state;		/// Pointer to job state.
	};

	/**
	 * Descriptor for a given job worker thread, to be provided by the host application.
	 */
	struct JobWorkerDescriptor
	{
		JobWorkerDescriptor()
		:	m_name					( "JobSystemWorker" )
		,	m_affinity				( 0xffffffff )
		,	m_enableWorkStealing	( true )
		{
		}

		std::string		m_name;						/// Worker name, for debug/profiling displays.
		unsigned int	m_affinity;					/// Thread affinity. Defaults to all cores.
		bool			m_enableWorkStealing : 1;	/// Enable queue-sharing between workers?
	};

	/**
	 * Job events (for tracking/debugging).
	 */
	enum EJobEvent
	{
		eJobEvent_JobPopped,			/// A job was popped from a queue.
		eJobEvent_JobStart,				/// A job is about to start.
		eJobEvent_JobDone,				/// A job just completed.
		eJobEvent_JobRun,				/// A job has been completed.
		eJobEvent_JobRunAssisted,		/// A job has been completed through outside assistance.
		eJobEvent_JobStolen,			/// A worker has stolen a job from another worker. 
		eJobEvent_WorkerAwoken,			/// A worker has been awoken.
		eJobEvent_WorkerUsed,			/// A worker has been utilized.
	};

	typedef std::function<void( const JobQueueEntry& job, EJobEvent, size_t, size_t )> JobEventObserver;	/// Delegate definition for job event observation.
	
	typedef std::deque<JobQueueEntry> JobQueue;		/// Data structure to represent job queue.

	/**
	 * High-res clock based on windows performance counter. Supports STL chrono interfaces.
	 */
	struct ProfileClock
	{
		typedef long long                               rep;
		typedef std::nano                               period;
		typedef std::chrono::duration<rep, period>      duration;
		typedef std::chrono::time_point<ProfileClock>   time_point;
		static const bool is_steady = true;

		static time_point now();
	};

	const long long g_Frequency = []() -> long long 
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		return frequency.QuadPart;
	}();

	ProfileClock::time_point ProfileClock::now()
	{
		LARGE_INTEGER count;
		QueryPerformanceCounter(&count);
		return time_point(duration(count.QuadPart * static_cast<rep>(period::den) / g_Frequency));
	}

	/**
	 * Tracking each job's start/end times in a per-worker timeline, for debugging/profiling.
	 */
	class ProfilingTimeline
	{
	public:

		struct TimelineEntry
		{
			size_t							jobId;					/// ID of the job that generated this timeline entry.
			ProfileClock::time_point		start;					/// Job start time.
			ProfileClock::time_point		end;					/// Job end time.
			char							debugChar;				/// Job's debug character for profiling display.
			
			std::string						description;			/// Timeline entry description.
		};

		typedef std::vector<TimelineEntry> TimelineEntries;

		TimelineEntries						m_entries;				/// List of timeline entries for this thread.
	};

	/**
	 * Represents a worker thread.
	 * - Owns a job queue
	 * - Implements work-stealing from other workers
	 */
	class JobSystemWorker
	{
		friend class JobManager;

	public:

		JobSystemWorker( const JobWorkerDescriptor& desc, const JobEventObserver& eventObserver )
		:	m_allWorkers		( nullptr )
		,	m_workerCount		( 0 )
		,	m_workerIndex		( 0 )
		,	m_desc				( desc )
		,	m_eventObserver		( eventObserver )
		,	m_hasShutDown		( false )
		{
		}

		void Start( size_t index, JobSystemWorker** allWorkers, size_t workerCount )
		{
			m_thread = std::thread( &JobSystemWorker::WorkerThreadProc, this );

			m_allWorkers = allWorkers;
			m_workerCount = workerCount;
			m_workerIndex = index;
		}

		void Shutdown( bool wait = true )
		{
			m_stop.store( true, std::memory_order_relaxed );

			while( !m_hasShutDown.load( std::memory_order_acquire ) )
			{
				s_signalThreads.notify_all();

				std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
			}

			if( m_hasShutDown.load( std::memory_order_acquire ) )
			{
				m_thread.join();
			}
		}

		JobStatePtr PushJob( JobDelegate delegate )
		{
			JobQueueEntry entry = { delegate, std::make_shared<JobState>() };
			entry.m_state->SetActive();

			{
				std::lock_guard<std::mutex> queueLock( m_queueLock );
				m_queue.insert( m_queue.begin(), entry );
			}

			return entry.m_state;
		}

	private:

		void NotifyEventObserver( const JobQueueEntry& job, EJobEvent event, size_t value, size_t value2 = 0 )
		{
			#ifdef JOBSYSTEM_ENABLE_PROFILING

			if( m_eventObserver )
			{
				m_eventObserver( job, event, value, value2 );
			}

			#endif // JOBSYSTEM_ENABLE_PROFILING
		}

		bool PopJobFromQueue( JobQueue& queue, JobQueueEntry& job, bool& hasUnsatisfiedDependencies )
		{
			for( auto jobIter = queue.begin(); jobIter != queue.end(); )
			{
				const JobQueueEntry& candidate = (*jobIter);

				if( candidate.m_state->AwaitingCancellation() )
				{
					job.m_state->SetDone();
					jobIter = queue.erase( jobIter );
				}
				else if( candidate.m_state->AreDependenciesMet() )
				{
					job = candidate;
					queue.erase( jobIter );

					NotifyEventObserver( job, eJobEvent_JobPopped, m_workerIndex );

					return true;
				}
				else
				{
					hasUnsatisfiedDependencies = true;
					++jobIter;
				}
			}

			return false;
		}

		bool PopNextJob( JobQueueEntry& job, bool& hasUnsatisfiedDependencies, bool useWorkStealing )
		{
			bool foundJob = false;

			{
				std::lock_guard<std::mutex> queueLock( m_queueLock );
				foundJob = PopJobFromQueue( m_queue, job, hasUnsatisfiedDependencies );
			}

			if( !foundJob && useWorkStealing )
			{
				for( size_t i = 0; foundJob == false && i < m_workerCount; ++i )
				{
					assert( m_allWorkers[i] );
					JobSystemWorker& worker = *m_allWorkers[i];
					
					{
						std::lock_guard<std::mutex> queueLock( worker.m_queueLock );
						foundJob = PopJobFromQueue( worker.m_queue, job, hasUnsatisfiedDependencies );
					}
				}

				if( foundJob )
				{
					NotifyEventObserver( job, eJobEvent_JobStolen, m_workerIndex );
				}
			}

			return foundJob;
		}

		void SetThreadName( const char* name )
		{
			#ifdef WINDOWS
			typedef struct tagTHREADNAME_INFO
			{
				unsigned long dwType; // must be 0x1000
				const char* szName; // pointer to name (in user addr space)
				unsigned long dwThreadID; // thread ID (-1=caller thread)
				unsigned long dwFlags; // reserved for future use, must be zero
			} THREADNAME_INFO;

			THREADNAME_INFO threadName;
			threadName.dwType = 0x1000;
			threadName.szName = m_desc.m_name.c_str();
			threadName.dwThreadID = GetCurrentThreadId();
			threadName.dwFlags = 0;
			__try
			{
				RaiseException( 0x406D1388, 0, sizeof(threadName)/sizeof(DWORD), (ULONG_PTR*)&threadName );
			}
			__except (EXCEPTION_CONTINUE_EXECUTION)
			{
			}
			#endif
		}

		void WorkerThreadProc()
		{
			SetThreadName( m_desc.m_name.c_str() );			

			#if defined(WINDOWS)
			SetThreadAffinityMask( m_thread.native_handle(), m_desc.m_affinity );
			#endif // WINDOWS

			while( true )
			{
				JobQueueEntry job;
				{
					std::unique_lock<std::mutex> signalLock( s_signalLock );

					bool hasUnsatisfiedDependencies;

					while(	!m_stop.load( std::memory_order_relaxed ) &&
							!PopNextJob( job, hasUnsatisfiedDependencies, m_desc.m_enableWorkStealing ) )
					{
						s_signalThreads.wait( signalLock );
						NotifyEventObserver( job, eJobEvent_WorkerAwoken, m_workerIndex );
					}
				}

				if( m_stop.load( std::memory_order_relaxed ) )
				{
					m_hasShutDown.store( true, std::memory_order_release );

					break;
				}

				s_activeWorkers.fetch_add( 1, std::memory_order_acq_rel );
				{
					NotifyEventObserver( job, eJobEvent_WorkerUsed, m_workerIndex );

					NotifyEventObserver( job, eJobEvent_JobStart, m_workerIndex, job.m_state->m_jobId );
					job.m_delegate();
					NotifyEventObserver( job, eJobEvent_JobDone, m_workerIndex );

					job.m_state->SetDone();

					NotifyEventObserver( job, eJobEvent_JobRun, m_workerIndex );

					s_signalThreads.notify_one();
				}
				s_activeWorkers.fetch_add( -1, std::memory_order_acq_rel );
			}
		}

		std::thread					m_thread;					/// Thread instance for worker.
		std::atomic<bool>			m_stop;						/// Has a stop been requested?
		std::atomic<bool>			m_hasShutDown;				/// Has the worker completed shutting down?

		mutable std::mutex			m_queueLock;				/// Mutex to guard worker queue.
		JobQueue					m_queue;					/// Queue containing requested jobs.

		JobSystemWorker**			m_allWorkers;				/// Pointer to array of all workers, for queue-sharing / work-stealing.
		size_t						m_workerCount;				/// Number of total workers (size of m_allWorkers array).
		size_t						m_workerIndex;				/// This worker's index within m_allWorkers.

		JobEventObserver			m_eventObserver;			/// Observer of job-related events occurring on this worker.
		JobWorkerDescriptor			m_desc;						/// Descriptor/configuration of this worker.
	};

	/**
	 * Descriptor for configuring the job manager.
	 * - Contains descriptor for each worker
	 */
	struct JobManagerDesciptor
	{
		std::vector<JobWorkerDescriptor>	m_workers;			/// Configurations for all workers that should be spawned by JobManager.
	};

	/**
	 * Manages job workers, and acts as the primary interface to the job queue.
	 */
	class JobManager
	{
	private:

		void Observer( const JobQueueEntry& job, EJobEvent event, size_t value, size_t value2 = 0 )
		{
			#ifdef JOBSYSTEM_ENABLE_PROFILING
			switch( event )
			{
			case eJobEvent_JobRun:
				{
					++m_jobsRun;
				}
				break;

			case eJobEvent_JobStolen:
				{
					++m_jobsStolen;
				}
				break;

			case eJobEvent_JobRunAssisted:
				{
					++m_jobsAssisted;
					++m_jobsRun;
				}
				break;

			case eJobEvent_WorkerAwoken:
				{
					m_awokenMask |= GetBit( value );
				}
				break;

			case eJobEvent_WorkerUsed:
				{
					m_usedMask |= GetBit( value );
				}
				break;

			case eJobEvent_JobStart:
				{
					auto& timeline = value < m_workers.size() ? m_timelines[ value ] : m_timelines[ m_workers.size() ];
					ProfilingTimeline::TimelineEntry entry;
					entry.jobId = value2;
					entry.start = ProfileClock::now();
					entry.debugChar = job.m_state ? job.m_state->m_debugChar : 0;
					timeline.m_entries.push_back( entry );
				}
				break;

			case eJobEvent_JobDone:
				{
					auto& timeline = value < m_workers.size() ? m_timelines[ value ] : m_timelines[ m_workers.size() ];
					auto& entry = timeline.m_entries.back();
					entry.end = ProfileClock::now();
				}
				break;

			case eJobEvent_JobPopped:
				{
					if( !m_hasPushedJob )
					{
						m_firstJobTime = ProfileClock::now();
						m_hasPushedJob = true;
					}
				}
				break;
			}
			#endif // JOBSYSTEM_ENABLE_PROFILING
		}

	public:

		JobManager()
		:	m_jobsRun					( 0 )
		,	m_jobsStolen				( 0 )
		,	m_usedMask					( 0 )
		,	m_awokenMask				( 0 )
		,	m_nextRoundRobinWorkerIndex	( 0 )
		,	m_timelines					( nullptr )
		,	m_firstJobTime				( )
		{

		}

		~JobManager()
		{
			DumpProfilingResults();

			Shutdown();
		}

		bool Create( const JobManagerDesciptor& desc )
		{
			Shutdown();

			m_desc = desc;

			const size_t workerCount = desc.m_workers.size();
			m_workers.reserve( workerCount );

			const JobEventObserver observer = std::bind( 
				&JobManager::Observer, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4 );

			// Create workers. We don't spawn the threads yet.
			for( size_t i = 0; i < workerCount; ++i )
			{
				const JobWorkerDescriptor& workerDesc = desc.m_workers[i];

				JobSystemWorker* worker = new JobSystemWorker( workerDesc, observer );
				m_workers.push_back( worker );
			}

			// Start the workers (includes spawning threads). Each worker maintains
			// understanding of what other workers exist, for work-stealing purposes.
			for( size_t i = 0; i < workerCount; ++i )
			{
				m_workers[i]->Start( i, &m_workers[0], workerCount );
			}

			#ifdef JOBSYSTEM_ENABLE_PROFILING

			m_timelines = new ProfilingTimeline[ workerCount + 1 ];
			m_hasPushedJob = false;

			#endif // JOBSYSTEM_ENABLE_PROFILING

			return !m_workers.empty();
		}

		JobStatePtr AddJob( JobDelegate delegate, char debugChar = 0 )
		{
			JobStatePtr state = nullptr;

			if( !m_workers.empty() )
			{
				// Add round-robin style. Note that work-stealing helps load-balance,
				// if it hasn't been disabled. If it has we may need to consider a
				// smarter scheme here.
				state = m_workers[ m_nextRoundRobinWorkerIndex ]->PushJob( delegate );
				state->m_debugChar = debugChar;

				m_nextRoundRobinWorkerIndex = ( m_nextRoundRobinWorkerIndex + 1 ) % m_workers.size();
			}

			return state;
		}

		void AssistUntilJobDone( JobStatePtr state )
		{
			assert( state->m_ready.load( std::memory_order_acquire ) );

			// Steal jobs from workers until the specified job is done.
			while( !state->IsDone() )
			{
				assert( !m_workers.empty() );
				
				JobQueueEntry job;
				bool hasUnsatisfiedDependencies;

				if( m_workers[0]->PopNextJob( job, hasUnsatisfiedDependencies, true ) )
				{
					Observer( job, eJobEvent_JobStart, m_workers.size(), job.m_state->m_jobId );
					job.m_delegate();
					Observer( job, eJobEvent_JobDone, m_workers.size() );

					job.m_state->SetDone();

					Observer( job, eJobEvent_JobRunAssisted, 0 );

					s_signalThreads.notify_one();
				}
				else
				{
					// No jobs available? This is either an error condition, or the assisting thread took the only job.
					std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
				}
			}
		}

		void AssistUntilDone()
		{
			assert( !m_workers.empty() );

			// Steal and run jobs from workers until all queues are exhausted.

			JobQueueEntry job;
			bool foundBusyWorker = true;

			while( foundBusyWorker )
			{
				foundBusyWorker = false;

				for( JobSystemWorker* worker : m_workers )
				{
					if( worker->PopNextJob( job, foundBusyWorker, false ) )
					{
						Observer( job, eJobEvent_JobStart, m_workers.size(), job.m_state->m_jobId );
						job.m_delegate();
						Observer( job, eJobEvent_JobDone, m_workers.size() );

						job.m_state->SetDone();

						Observer( job, eJobEvent_JobRunAssisted, 0 );

						foundBusyWorker = true;
						s_signalThreads.notify_one();
						break;
					}
				}
			}

			for( JobSystemWorker* worker : m_workers )
			{
				if( !worker->m_queue.empty() )
				{
					assert( 0 );
				}
			}
		}

		void Shutdown( bool finishJobs = false )
		{
			if( finishJobs )
			{
				AssistUntilDone();
			}

			// Tear down each worker. Un-popped jobs may still reside in the queues at this point
			// if finishJobs = false.
			// Don't destruct workers yet, in case someone's in the process of work-stealing.
			for( size_t i = 0, n = m_workers.size(); i < n; ++i )
			{
				assert( m_workers[i] );
				m_workers[i]->Shutdown( true );
			}

			// Destruct all workers.
			std::for_each( m_workers.begin(), m_workers.end(), []( JobSystemWorker* worker ) { delete worker; } );
			m_workers.clear();

			#ifdef JOBSYSTEM_ENABLE_PROFILING

			delete [] m_timelines;
			m_timelines = nullptr;

			#endif // JOBSYSTEM_ENABLE_PROFILING
		}

	private:
		
		size_t							m_nextRoundRobinWorkerIndex;			/// Index of the worker to receive the next requested job, round-robin style.

		std::atomic<unsigned int>		m_jobsRun;								/// Counter to track # of jobs run.
		std::atomic<unsigned int>		m_jobsAssisted;							/// Counter to track # of jobs run via external Assist*().
		std::atomic<unsigned int>		m_jobsStolen;							/// Counter to track # of jobs stolen from another worker's queue.
		std::atomic<unsigned int>		m_usedMask;								/// Mask with bits set according to the IDs of the jobs that have executed jobs.
		std::atomic<unsigned int>		m_awokenMask;							/// Mask with bits set according to the IDs of the jobs that have been awoken at least once.

	private:

		JobManagerDesciptor				m_desc;									/// Descriptor/configuration of the job manager.
		
		bool							m_hasPushedJob;							/// For profiling - has a job been pushed yet?
		ProfileClock::time_point		m_firstJobTime;							/// For profiling - when was the first job pushed?
		ProfilingTimeline*				m_timelines;							/// For profiling - a ProfilingTimeline entry for each worker, plus an additional entry to represent the Assist thread.

		std::vector<JobSystemWorker*>	m_workers;								/// Storage for worker instances.

		void DumpProfilingResults()
		{
			#ifdef JOBSYSTEM_ENABLE_PROFILING

			AssistUntilDone();

			auto now = ProfileClock::now();
			auto totalNS = std::chrono::duration_cast<std::chrono::nanoseconds>( now - m_firstJobTime ).count();

			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

			const size_t workerCount = m_workers.size();

			char status[512];
			sprintf_s( status,
				"\n[Job System Statistics]\n"
				"Jobs Run:       %8d\n" // May be < jobs submitted
				"Jobs Stolen:    %8d\n"
				"Jobs Assisted:  %8d\n"
				"Workers Used:   %p\n"
				"Workers Awoken: %p\n"
				,
				m_jobsRun.load( std::memory_order_acquire ),
				m_jobsStolen.load( std::memory_order_acquire ),
				m_jobsAssisted.load( std::memory_order_acquire ),
				m_usedMask.load( std::memory_order_acquire ),
				m_awokenMask.load( std::memory_order_acquire ) );

			printf( status );
			OutputDebugStringA( status );

			{
				char buffer[128];
				sprintf_s( buffer, "\n[Worker Profiling Results]\n%.3f total ms, %.3f ms per tick\n\n", float(totalNS) / 1000.f / 1000.f, float(totalNS) / 1000.f / 1000.f / 199.f );
				OutputDebugStringA( buffer );
			}

			const char* busySymbols = "abcdefghijklmn";
			const size_t busySymbolCount = strlen( busySymbols );

			for( size_t i = 0; i < workerCount + 1; ++i )
			{
				auto& timeline = m_timelines[i];

				const char* name = ( i < workerCount ) ? m_workers[i]->m_desc.m_name.c_str() : "[Assist]";

				const size_t bufferSize = 200;
				char buffer[ bufferSize ];
				sprintf_s( buffer, "%20s: ", name );

				const size_t nameLen = strlen( buffer );
				const size_t remaining = bufferSize - nameLen - 2;
	
				for( size_t i = nameLen; i < bufferSize - 2; ++i )
					buffer[i] = '-';
				buffer[bufferSize - 2] = '\n';
				buffer[bufferSize - 1] = 0;

				for( auto& entry : timeline.m_entries )
				{
					auto startNs = std::chrono::duration_cast<std::chrono::nanoseconds>( entry.start - m_firstJobTime ).count();
					auto endNs = std::chrono::duration_cast<std::chrono::nanoseconds>( entry.end - m_firstJobTime ).count();

					const float startPercent = ( float( startNs ) / float( totalNS ) );
					const float endPercent = ( float( endNs ) / float( totalNS ) );

					char jobCharacter = ( entry.debugChar != 0 ) ? entry.debugChar : busySymbols[ entry.jobId % busySymbolCount ];

					size_t startIndex = nameLen + std::min<size_t>( remaining - 1, size_t( startPercent * float(remaining) ) );
					size_t endIndex = nameLen + std::min<size_t>( remaining - 1, size_t( endPercent * float(remaining) ) );

					size_t shift = 0;

					while( buffer[startIndex+shift] != '-' && startIndex+shift < bufferSize - 3 && endIndex+shift < bufferSize - 3 )
					{
						++shift;
					}

					endIndex -= std::min( endIndex - startIndex, shift );

					for( size_t i = startIndex+shift; i <= endIndex+shift; ++i )
					{
						assert( i < bufferSize - 2 );
						buffer[i] = jobCharacter;
					}
				}

				OutputDebugStringA( buffer );
			}

			OutputDebugStringA( "\n" );

			#endif // JOBSYSTEM_ENABLE_PROFILING
		}
	};

	/**
	 * Helper for building complex job/dependency chains logically.
	 *
	 * e.g.
	 *
	 * jobsystem::JobChainBuilder<256>( manager )
	 *	.Do( Something )
	 *	.Then()
	 *	.Do( SomethingAfterThat )
	 *	.Then()
	 *	.Together()
	 *		.Do( Thing1 )
	 *		.Do( Thing2 )
	 *		.Do( Thing3 )
	 *	.Close()
	 *	.Then()
	 *	.Do( FinalStuff );
	 *	.Go();
	 **									  --- Thing 1 ---
	 *									 /				 \
	 * Something -> SomethingAfterThat -> --- Thing 2 ---- -> FinalStuff
	 *									 \				 /
	 *									  --- Thing 3 ---
	 * etc...
	 * 
	 */
	template<size_t MaxJobNodes = 256>
	class JobChainBuilder
	{
	public:

		struct Node
		{
			Node() : isGroup(false), groupDependency(nullptr) {}
			~Node() {}

			Node*			groupDependency;
			JobStatePtr		job;
			bool			isGroup;
		};

		Node* AllocNode()
		{
			if( m_nextNodeIndex >= MaxJobNodes )
				return nullptr;

			Node* node = &m_nodePool[ m_nextNodeIndex++ ];
			*node = Node();

			return node;
		}

		JobChainBuilder( JobManager& manager )
		:	mgr				( manager )
		{
			Reset();

			// Push a sentinel (root) node.
			m_stack.push_back( AllocNode() );
		}

		void Reset()
		{		
			m_allJobs.clear();
			m_stack.clear();

			m_last = nullptr;
			m_dependency = nullptr;
			m_nextNodeIndex = 0;
			m_failed = false;
		}

		JobChainBuilder& Together( char debugChar = 0 )
		{
			if( Node* item = AllocNode() )
			{
				item->isGroup = true;
				item->groupDependency = m_dependency;

				item->job = mgr.AddJob( [](){}, debugChar );

				m_allJobs.push_back( item->job );

				m_last = item;
				m_dependency = nullptr;

				m_stack.push_back( item );
			}
			else
			{
				Fail();
			}

			return *this;
		}

		JobChainBuilder& Do( JobDelegate delegate, char debugChar = 0 )
		{
			Node* owner = m_stack.back();

			if( Node* item = AllocNode() )
			{
				item->job = mgr.AddJob( delegate, debugChar );

				m_allJobs.push_back( item->job );

				if( m_dependency )
				{
					m_dependency->job->AddDependant( item->job );
					m_dependency = nullptr;
				}

				if( owner && owner->isGroup )
				{
					item->job->AddDependant( owner->job );

					if( owner->groupDependency )
					{
						owner->groupDependency->job->AddDependant( item->job );
					}
				}

				m_last = item;
			}
			else
			{
				Fail();
			}

			return *this;
		}

		JobChainBuilder& Then()
		{			
			m_dependency = m_last;
			m_last = ( m_dependency ) ? m_dependency->groupDependency : nullptr;

			return *this;
		}

		JobChainBuilder& Close()
		{	
			if( !m_stack.empty() )
			{
				Node* owner = m_stack.back();
				if( owner->isGroup )
				{
					m_last = owner;
				}
			}

			m_dependency = nullptr;
		
			if( m_stack.size() > 1 )
			{
				m_stack.pop_back();
			}

			return *this;
		}

		void Go()
		{
			for( auto job : m_allJobs )
			{
				job->SetReady();
			}
		}

		void Fail()
		{
			for( auto job : m_allJobs )
			{
				job->Cancel();
			}

			m_failed = true;
		}

		bool Failed() const
		{
			return m_failed;
		}

		JobManager&					mgr;						/// Job manager to submit jobs to.
		
		Node						m_nodePool[ MaxJobNodes ];	/// Pool of chain nodes (on the stack). The only necessary output of this system is jobs. Nodes are purely internal.
		size_t						m_nextNodeIndex;			/// Next free item in the pool.

		std::vector<Node*>			m_stack;					/// Internal stack to track groupings.
		std::vector<JobStatePtr>	m_allJobs;					/// All jobs created by the builder, to be readied on completion.

		Node*						m_last;						/// Last job to be pushed, to handle setting up dependencies after Then() calls.
		Node*						m_dependency;				/// Any job promoted to a dependency for the next job, as dicated by Then().

		bool						m_failed;					/// Did an error occur during creation of the DAG?
	};


} // namespace jobsystem

#endif // COMMONJOBSYSTEM_HEADER