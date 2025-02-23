/* Copyright (c) 2005, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef RPL_RLI_H
#define RPL_RLI_H

#include "sql_priv.h"
#include "rpl_info.h"
#include "rpl_utility.h"
#include "rpl_tblmap.h"
#include "rpl_reporting.h"
#include "rpl_utility.h"
#include "log.h"                         /* LOG_INFO */
#include "binlog.h"                      /* MYSQL_BIN_LOG */
#include "sql_class.h"                   /* THD */

#if defined(HAVE_REPLICATION) && !defined(MYSQL_CLIENT)
#include "log_event_wrapper.h"
#endif // HAVE_REPLICATION and !MYSQL_CLIENT

#include <atomic>
#include <deque>

struct RPL_TABLE_LIST;
class Master_info;
class Commit_order_manager;
extern uint sql_slave_skip_counter;


enum class Enum_slave_caughtup {
  NONE,
  YES,
  NO,
};

/*******************************************************************************
Replication SQL Thread

Relay_log_info contains:
  - the current relay log
  - the current relay log offset
  - master log name
  - master log sequence corresponding to the last update
  - misc information specific to the SQL thread

Relay_log_info is initialized from a repository, i.e. table or file, if there is
one. Otherwise, data members are intialized with defaults by calling
init_relay_log_info().

The relay.info table/file shall be updated whenever: (i) the relay log file
is rotated, (ii) SQL Thread is stopped, (iii) while processing a Xid_log_event,
(iv) after a Query_log_event (i.e. commit or rollback) and (v) after processing
any statement written to the binary log without a transaction context.

The Xid_log_event is a commit for transactional engines and must be handled
differently to provide reliability/data integrity. In this case, positions
are updated within the context of the current transaction. So

  . If the relay.info is stored in a transactional repository and the server
  crashes before successfully committing the transaction the changes to the
  position table will be rolled back along with the data.

  . If the relay.info is stored in a non-transactional repository, for instance,
  a file or a system table created using MyIsam, and the server crashes before
  successfully committing the transaction the changes to the position table
  will not be rolled back but data will.

In particular, when there are mixed transactions, i.e a transaction that updates
both transaction and non-transactional engines, the Xid_log_event is still used
but reliability/data integrity cannot be achieved as we shall explain in what
follows.

Changes to non-transactional engines, such as MyIsam, cannot be rolled back if a
failure happens. For that reason, there is no point in updating the positions
within the boundaries of any on-going transaction. This is true for both commit
and rollback. If a failure happens after processing the pseudo-transaction but
before updating the positions, the transaction will be re-executed when the
slave is up most likely causing an error that needs to be manually circumvented.
This is a well-known issue when non-transactional statements are executed.

Specifically, if rolling back any transaction, positions are updated outside the
transaction boundaries. However, there may be a problem in this scenario even
when only transactional engines are updated. This happens because if there is a
rollback and such transaction is written to the binary log, a non-transactional
engine was updated or a temporary table was created or dropped within its
boundaries.

In particular, in both STATEMENT and MIXED logging formats, this happens because
any temporary table is automatically dropped after a shutdown/startup.
See BUG#26945 for further details.

Statements written to the binary log outside the boundaries of a transaction are
DDLs or maintenance commands which are not transactional. These means that they
cannot be rolled back if a failure happens. In such cases, the positions are
updated after processing the events. If a failure happens after processing the
statement but before updating the positions, the statement will be
re-executed when the slave is up most likely causing an error that needs to be
manually circumvented. This is a well-known issue when non-transactional
statements are executed.

The --sync-relay-log-info does not have effect when a system table, either
transactional or non-transactional is used.

To correctly recovery from failures, one should combine transactional system
tables along with the --relay-log-recovery.
*******************************************************************************/
class Relay_log_info : public Rpl_info
{
  friend class Rpl_info_factory;

public:
  /**
     Flags for the state of the replication.
   */
  enum enum_state_flag {
    /** The replication thread is inside a statement */
    IN_STMT,

    /** Flag counter.  Should always be last */
    STATE_FLAGS_COUNT
  };

  /*
    The SQL thread owns one Relay_log_info, and each client that has
    executed a BINLOG statement owns one Relay_log_info. This function
    returns zero for the Relay_log_info object that belongs to the SQL
    thread and nonzero for Relay_log_info objects that belong to
    clients.
  */
  inline bool belongs_to_client()
  {
    DBUG_ASSERT(info_thd);
    return !info_thd->slave_thread;
  }

  /*
    If true, events with the same server id should be replicated. This
    field is set on creation of a relay log info structure by copying
    the value of ::replicate_same_server_id and can be overridden if
    necessary. For example of when this is done, check sql_binlog.cc,
    where the BINLOG statement can be used to execute "raw" events.
   */
  bool replicate_same_server_id;

  /*** The following variables can only be read when protect by data lock ****/
  /*
    cur_log_fd - file descriptor of the current read  relay log
  */
  File cur_log_fd;
  /*
    Protected with internal locks.
    Must get data_lock when resetting the logs.
  */
  MYSQL_BIN_LOG relay_log;
  LOG_INFO linfo;

  /*
   cur_log
     Pointer that either points at relay_log.get_log_file() or
     &rli->cache_buf, depending on whether the log is hot or there was
     the need to open a cold relay_log.

   cache_buf 
     IO_CACHE used when opening cold relay logs.
   */
  IO_CACHE cache_buf,*cur_log;

  /*
    Identifies when the recovery process is going on.
    See sql/slave.cc:init_recovery for further details.
  */
  bool is_relay_log_recovery;

  Gtid recovery_max_engine_gtid;
  Sid_map *recovery_sid_map;
  Checkable_rwlock *recovery_sid_lock;

  DYNAMIC_ARRAY gtid_infos;
  // global hash to store the slave gtid_info repositories mapped by db names.
  HASH map_db_to_gtid_info;
  /**
    Reader writer lock to protect map_db_to_gtid_info. The hash
    is updated only by coordinator thread. slave worker threads only
    search in this hash.
  */
  mysql_rwlock_t gtid_info_hash_lock;
  // Last gtid seen by coordinator thread.
  char last_gtid[Gtid::MAX_TEXT_LENGTH + 1];
  bool gtid_info_hash_inited;
  bool part_event;  // true if the current event contains partition event.
  bool ends_group;
  // next available id for new gtid info
  uint gtid_info_next_id;

  /* The following variables are safe to read any time */

  /*
    When we restart slave thread we need to have access to the previously
    created temporary tables. Modified only on init/end and by the SQL
    thread, read only by SQL thread.
  */
  TABLE *save_temporary_tables;

  /* parent Master_info structure */
  Master_info *mi;

  /*
    Needed to deal properly with cur_log getting closed and re-opened with
    a different log under our feet
  */
  uint32 cur_log_old_open_count;

  /*
    If on init_info() call error_on_rli_init_info is true that means
    that previous call to init_info() terminated with an error, RESET
    SLAVE must be executed and the problem fixed manually.
   */
  bool error_on_rli_init_info;

  /*
    Let's call a group (of events) :
      - a transaction
      or
      - an autocommiting query + its associated events (INSERT_ID,
    TIMESTAMP...)
    We need these rli coordinates :
    - relay log name and position of the beginning of the group we currently are
    executing. Needed to know where we have to restart when replication has
    stopped in the middle of a group (which has been rolled back by the slave).
    - relay log name and position just after the event we have just
    executed. This event is part of the current group.
    Formerly we only had the immediately above coordinates, plus a 'pending'
    variable, but this dealt wrong with the case of a transaction starting on a
    relay log and finishing (commiting) on another relay log. Case which can
    happen when, for example, the relay log gets rotated because of
    max_binlog_size.
  */
protected:
  char group_relay_log_name[FN_REFLEN];
  ulonglong group_relay_log_pos;
  char event_relay_log_name[FN_REFLEN];
  ulonglong event_relay_log_pos;
  ulonglong future_event_relay_log_pos;

  /*
     Original log name and position of the group we're currently executing
     (whose coordinates are group_relay_log_name/pos in the relay log)
     in the master's binlog. These concern the *group*, because in the master's
     binlog the log_pos that comes with each event is the position of the
     beginning of the group.

    Note: group_master_log_name, group_master_log_pos must only be
    written from the thread owning the Relay_log_info (SQL thread if
    !belongs_to_client(); client thread executing BINLOG statement if
    belongs_to_client()).
  */
  char group_master_log_name[FN_REFLEN];
  volatile my_off_t group_master_log_pos;

  /*
    When it commits, InnoDB internally stores the master log position it has
    processed so far; the position to store is the one of the end of the
    committing event (the COMMIT query event, or the event if in autocommit
    mode).
  */
#if MYSQL_VERSION_ID < 40100
  ulonglong future_master_log_pos;
#else
  ulonglong future_group_master_log_pos;
#endif

private:
  Gtid_set gtid_set;
  /* Last gtid retrieved by IO thread */
  Gtid last_retrieved_gtid;

public:
  Gtid *get_last_retrieved_gtid() { return &last_retrieved_gtid; }
  void set_last_retrieved_gtid(Gtid gtid) { last_retrieved_gtid= gtid; }
  int add_logged_gtid(rpl_sidno sidno, rpl_gno gno)
  {
    int ret= 0;
    global_sid_lock->assert_some_lock();
    DBUG_ASSERT(sidno <= global_sid_map->get_max_sidno());
    gtid_set.ensure_sidno(sidno);
    if (gtid_set._add_gtid(sidno, gno) != RETURN_STATUS_OK)
      ret= 1;
    return ret;
  }
  const Gtid_set *get_gtid_set() const { return &gtid_set; }

  int init_relay_log_pos(const char* log,
                         ulonglong pos, bool need_data_lock,
                         const char** errmsg,
                         bool keep_looking_for_fd);

  /*
    Handling of the relay_log_space_limit optional constraint.
    ignore_log_space_limit is used to resolve a deadlock between I/O and SQL
    threads, the SQL thread sets it to unblock the I/O thread and make it
    temporarily forget about the constraint.
  */
  ulonglong log_space_limit;
  std::atomic_ullong log_space_total;
  bool ignore_log_space_limit;

  /*
    Used by the SQL thread to instructs the IO thread to rotate 
    the logs when the SQL thread needs to purge to release some
    disk space.
   */
  bool sql_force_rotate_relay;
  /*
    A flag to say "consider we have caught up" when calculating seconds behind
    the master. This value is initialized to NONE during startup and SBM is
    set to NULL. If slave_has_caughtup is YES, SBM is set to 0.
  */
  Enum_slave_caughtup slave_has_caughtup;

  // NOTE: a copy is also maintained in MYSQL_BIN_LOG
  time_t last_master_timestamp;

  // cache value for sql thread
  time_t penultimate_master_timestamp;

  // last master timestamp in milli seconds from trx meta data
  ulonglong last_master_timestamp_millis= 0;

  // milli ts for the current group
  ulonglong group_timestamp_millis= 0;

  void set_last_master_timestamp(time_t ts, ulonglong ts_millis);

#define PEAK_LAG_MAX_SECS 512
  time_t peak_lag_last[PEAK_LAG_MAX_SECS];
  ulong events_since_last_sample;

  void update_peak_lag(time_t when_master);

  time_t peak_lag(time_t now);

  void clear_until_condition();

  /**
    Reset the delay.
    This is used by RESET SLAVE to clear the delay.
  */
  void clear_sql_delay()
  {
    sql_delay= 0;
  }

  /*
    Needed for problems when slave stops and we want to restart it
    skipping one or more events in the master log that have caused
    errors, and have been manually applied by DBA already.
  */
  volatile uint32 slave_skip_counter;
  volatile ulong abort_pos_wait;	/* Incremented on change master */
  mysql_mutex_t log_space_lock;
  mysql_cond_t log_space_cond;

  /*
     Condition and its parameters from START SLAVE UNTIL clause.
     
     UNTIL condition is tested with is_until_satisfied() method that is
     called by exec_relay_log_event(). is_until_satisfied() caches the result
     of the comparison of log names because log names don't change very often;
     this cache is invalidated by parts of code which change log names with
     notify_*_log_name_updated() methods. (They need to be called only if SQL
     thread is running).
   */
  enum {UNTIL_NONE= 0, UNTIL_MASTER_POS, UNTIL_RELAY_POS,
        UNTIL_SQL_BEFORE_GTIDS, UNTIL_SQL_AFTER_GTIDS,
        UNTIL_SQL_AFTER_MTS_GAPS, UNTIL_DONE
}
    until_condition;
  char until_log_name[FN_REFLEN];
  ulonglong until_log_pos;
  /* extension extracted from log_name and converted to int */
  ulong until_log_name_extension;
  /**
    The START SLAVE UNTIL SQL_*_GTIDS initializes until_sql_gtids.
    Each time a gtid is about to be processed, we check if it is in the
    set. Depending on until_condition, SQL thread is stopped before or
    after applying the gtid.
  */
  Gtid_set until_sql_gtids;
  /*
    True if the current event is the first gtid event to be processed
    after executing START SLAVE UNTIL SQL_*_GTIDS.
  */
  bool until_sql_gtids_first_event;
  /* 
     Cached result of comparison of until_log_name and current log name
     -2 means unitialised, -1,0,1 are comarison results 
  */
  enum 
  { 
    UNTIL_LOG_NAMES_CMP_UNKNOWN= -2, UNTIL_LOG_NAMES_CMP_LESS= -1,
    UNTIL_LOG_NAMES_CMP_EQUAL= 0, UNTIL_LOG_NAMES_CMP_GREATER= 1
  } until_log_names_cmp_result;

  char cached_charset[6];
  /*
    trans_retries varies between 0 to slave_transaction_retries and counts how
    many times the slave has retried the present transaction; gets reset to 0
    when the transaction finally succeeds. retried_trans is a cumulative
    counter: how many times the slave has retried a transaction (any) since
    slave started.
  */
  ulong trans_retries, retried_trans;

  /*
    If the end of the hot relay log is made of master's events ignored by the
    slave I/O thread, these two keep track of the coords (in the master's
    binlog) of the last of these events seen by the slave I/O thread. If not,
    ign_master_log_name_end[0] == 0.
    As they are like a Rotate event read/written from/to the relay log, they
    are both protected by rli->relay_log.LOCK_log.
  */
  char ign_master_log_name_end[FN_REFLEN];
  ulonglong ign_master_log_pos_end;

  /* 
    Indentifies where the SQL Thread should create temporary files for the
    LOAD DATA INFILE. This is used for security reasons.
   */ 
  char slave_patternload_file[FN_REFLEN]; 
  size_t slave_patternload_file_size;

  /**
    Identifies the last time a checkpoint routine has been executed.
  */
  struct timespec last_clock;

  /**
    Invalidates cached until_log_name and group_relay_log_name comparison
    result. Should be called after any update of group_realy_log_name if
    there chances that sql_thread is running.
  */
  inline void notify_group_relay_log_name_update()
  {
    if (until_condition==UNTIL_RELAY_POS)
      until_log_names_cmp_result= UNTIL_LOG_NAMES_CMP_UNKNOWN;
  }

  /**
    The same as @c notify_group_relay_log_name_update but for
    @c group_master_log_name.
  */
  inline void notify_group_master_log_name_update()
  {
    if (until_condition==UNTIL_MASTER_POS)
      until_log_names_cmp_result= UNTIL_LOG_NAMES_CMP_UNKNOWN;
  }
  
  inline void inc_event_relay_log_pos()
  {
    event_relay_log_pos= future_event_relay_log_pos;
  }

  int inc_group_relay_log_pos(ulonglong log_pos,
                              bool need_data_lock);

  int wait_for_pos(THD* thd, String* log_name, longlong log_pos, 
		   longlong timeout);
  int wait_for_gtid_set(THD* thd, String* gtid, longlong timeout);
  void close_temporary_tables();

  /* Check if UNTIL condition is satisfied. See slave.cc for more. */
  bool is_until_satisfied(THD *thd, Log_event *ev);
  inline ulonglong until_pos()
  {
    return ((until_condition == UNTIL_MASTER_POS) ? group_master_log_pos :
	    group_relay_log_pos);
  }

  RPL_TABLE_LIST *tables_to_lock;           /* RBR: Tables to lock  */
  uint tables_to_lock_count;        /* RBR: Count of tables to lock */
  table_mapping m_table_map;      /* RBR: Mapping table-id to table */
  /* RBR: Record Rows_query log event */
  Rows_query_log_event* rows_query_ev;
  /* Meta data about the current trx from the master */
  std::string trx_meta_data_json;

  bool get_table_data(TABLE *table_arg, table_def **tabledef_var, TABLE **conv_table_var) const
  {
    DBUG_ASSERT(tabledef_var && conv_table_var);
    for (TABLE_LIST *ptr= tables_to_lock ; ptr != NULL ; ptr= ptr->next_global)
      if (ptr->table == table_arg)
      {
        *tabledef_var= &static_cast<RPL_TABLE_LIST*>(ptr)->m_tabledef;
        *conv_table_var= static_cast<RPL_TABLE_LIST*>(ptr)->m_conv_table;
        DBUG_PRINT("debug", ("Fetching table data for table %s.%s:"
                             " tabledef: %p, conv_table: %p",
                             table_arg->s->db.str, table_arg->s->table_name.str,
                             *tabledef_var, *conv_table_var));
        return true;
      }
    return false;
  }

  /**
    Last charset (6 bytes) seen by slave SQL thread is cached here; it helps
    the thread save 3 @c get_charset() per @c Query_log_event if the charset is not
    changing from event to event (common situation).
    When the 6 bytes are equal to 0 is used to mean "cache is invalidated".
  */
  void cached_charset_invalidate();
  bool cached_charset_compare(char *charset) const;

  void cleanup_context(THD *, bool);
  void slave_close_thread_tables(THD *);
  void clear_tables_to_lock();
  int purge_relay_logs(THD *thd, bool just_reset, const char** errmsg);

  /*
    Used to defer stopping the SQL thread to give it a chance
    to finish up the current group of events.
    The timestamp is set and reset in @c sql_slave_killed().
  */
  time_t last_event_start_time;
  /*
    A container to hold on Intvar-, Rand-, Uservar- log-events in case
    the slave is configured with table filtering rules.
    The withhold events are executed when their parent Query destiny is
    determined for execution as well.
  */
  Deferred_log_events *deferred_events;

  /*
    State of the container: true stands for IRU events gathering, 
    false does for execution, either deferred or direct.
  */
  bool deferred_events_collecting;

  /*****************************************************************************
    WL#5569 MTS

    legends:
    C  - Coordinator;
    W  - Worker;
    WQ - Worker Queue containing event assignments
  */
  DYNAMIC_ARRAY workers; // number's is determined by global slave_parallel_workers
  volatile ulong pending_jobs;
  mysql_mutex_t pending_jobs_lock;
  mysql_cond_t pending_jobs_cond;
  mysql_mutex_t exit_count_lock; // mutex of worker exit count
  ulong       mts_slave_worker_queue_len_max;
  ulonglong   mts_pending_jobs_size;      // actual mem usage by WQ:s
  ulonglong   mts_pending_jobs_size_max;  // max of WQ:s size forcing C to wait
  bool    mts_wq_oversize;      // C raises flag to wait some memory's released
  Slave_worker  *last_assigned_worker;// is set to a Worker at assigning a group
  /*
    master-binlog ordered queue of Slave_job_group descriptors of groups
    that are under processing. The queue size is @c checkpoint_group.
  */
  Slave_committed_queue *gaq;
  /*
    Container for references of involved partitions for the current event group
  */
  DYNAMIC_ARRAY curr_group_assigned_parts;
  DYNAMIC_ARRAY curr_group_da;  // deferred array to hold partition-info-free events
  bool curr_group_seen_gtid;   // current group started with Gtid-event or not
  bool curr_group_seen_begin;   // current group started with B-event or not
  bool curr_group_seen_metadata; // Have we encountered metadata event
  bool curr_group_isolated;     // current group requires execution in isolation
  bool mts_end_group_sets_max_dbs; // flag indicates if partitioning info is discovered
  volatile ulong mts_wq_underrun_w_id;  // Id of a Worker whose queue is getting empty
  /* 
     Ongoing excessive overrun counter to correspond to number of events that
     are being scheduled while a WQ is close to be filled up.
     `Close' is defined as (100 - mts_worker_underrun_level) %.
     The counter is incremented each time a WQ get filled over that level
     and decremented when the level drops below.
     The counter therefore describes level of saturation that Workers 
     are experiencing and is used as a parameter to compute a nap time for
     Coordinator in order to avoid reaching WQ limits.
  */
  volatile long mts_wq_excess_cnt;
  long  mts_worker_underrun_level; // % of WQ size at which W is considered hungry
  ulong mts_coordinator_basic_nap; // C sleeps to avoid WQs overrun
  ulong opt_slave_parallel_workers; // cache for ::opt_slave_parallel_workers
  ulong slave_parallel_workers; // the one slave session time number of workers
  ulong exit_counter; // Number of workers contributed to max updated group index
  ulonglong max_updated_index;
  ulong recovery_parallel_workers; // number of workers while recovering
  uint checkpoint_seqno;  // counter of groups executed after the most recent CP
  uint checkpoint_group;  // cache for ::opt_mts_checkpoint_group
  MY_BITMAP recovery_groups;  // bitmap used during recovery
  bool recovery_groups_inited;
  ulong mts_recovery_group_cnt; // number of groups to execute at recovery
  ulong mts_recovery_index;     // running index of recoverable groups
  bool mts_recovery_group_seen_begin;

  /*
    While distibuting events basing on their properties MTS
    Coordinator changes its mts group status.
    Transition normally flowws to follow `=>' arrows on the diagram:

            +----------------------------+
            V                            |
    MTS_NOT_IN_GROUP =>                  |
        {MTS_IN_GROUP => MTS_END_GROUP --+} while (!killed) => MTS_KILLED_GROUP
      
    MTS_END_GROUP has `->' loop breaking link to MTS_NOT_IN_GROUP when
    Coordinator synchronizes with Workers by demanding them to
    complete their assignments.
  */
  enum
  {
    /* 
       no new events were scheduled after last synchronization,
       includes Single-Threaded-Slave case.
    */
    MTS_NOT_IN_GROUP,

    MTS_IN_GROUP,    /* at least one not-terminal event scheduled to a Worker */
    MTS_END_GROUP,   /* the last scheduled event is a terminal event */
    MTS_KILLED_GROUP /* Coordinator gave up to reach MTS_END_GROUP */
  } mts_group_status;

  /*
    MTS statistics:
  */
  ulonglong mts_events_assigned; // number of events (statements) scheduled
  ulonglong mts_groups_assigned; // number of groups (transactions) scheduled
  volatile ulong mts_wq_overrun_cnt; // counter of all mts_wq_excess_cnt increments
  ulong wq_size_waits_cnt;    // number of times C slept due to WQ:s oversize
  /*
    a counter for sleeps due to Coordinator 
    experienced waiting when Workers get hungry again
  */
  ulong mts_wq_no_underrun_cnt;
  ulong mts_wq_overfill_cnt;  // counter of C waited due to a WQ queue was full
  /* 
     A sorted array of the Workers' current assignement numbers to provide
     approximate view on Workers loading.
     The first row of the least occupied Worker is queried at assigning 
     a new partition. Is updated at checkpoint commit to the main RLI.
  */
  DYNAMIC_ARRAY least_occupied_workers;
  time_t mts_last_online_stat;
  /* end of MTS statistics */

  /* most of allocation in the coordinator rli is there */
  void init_workers(ulong);

  /* counterpart of the init */
  void deinit_workers();

  void init_gtid_infos();

  void deinit_gtid_infos();

  inline void gtid_info_hash_rdlock()
  {
    mysql_rwlock_rdlock(&gtid_info_hash_lock);
  }
  inline void gtid_info_hash_wrlock()
  {
    mysql_rwlock_wrlock(&gtid_info_hash_lock);
  }
  inline void gtid_info_hash_unlock()
  {
    mysql_rwlock_unlock(&gtid_info_hash_lock);
  }
  int flush_gtid_infos(bool force, bool xid_event = false);
  /**
     returns true if there is any gap-group of events to execute
                  at slave starting phase.
  */
  inline bool is_mts_recovery() const
  {
    return mts_recovery_group_cnt != 0;
  }

  inline void clear_mts_recovery_groups()
  {
    if (recovery_groups_inited)
    {
      bitmap_free(&recovery_groups);
      mts_recovery_group_cnt= 0;
      recovery_groups_inited= false;
    }
  }

  /**
     returns true if events are to be executed in parallel
  */
  inline bool is_parallel_exec() const
  {
    bool ret= (slave_parallel_workers > 0) && !is_mts_recovery();

    DBUG_ASSERT(!ret || workers.elements > 0);

    return ret;
  }

  /**
     returns true if Coordinator is scheduling events belonging to
     the same group and has not reached yet its terminal event.
  */
  inline bool is_mts_in_group()
  {
    return is_parallel_exec() &&
      mts_group_status == MTS_IN_GROUP;
  }

  bool mts_workers_queue_empty();
  bool cannot_safely_rollback();

  /**
     While a group is executed by a Worker the relay log can change.
     Coordinator notifies Workers about this event. Worker is supposed
     to commit to the recovery table with the new info.
  */
  void reset_notified_relay_log_change();

  /**
     While a group is executed by a Worker the relay log can change.
     Coordinator notifies Workers about this event. Coordinator and Workers
     maintain a bitmap of executed group that is reset with a new checkpoint. 
  */
  void reset_notified_checkpoint(ulong, time_t, ulonglong, bool);

  /**
     Called when gaps execution is ended so it is crash-safe
     to reset the last session Workers info.
  */
  bool mts_finalize_recovery();
  /*
   * End of MTS section ******************************************************/

  /* The general cleanup that slave applier may need at the end of query. */
  inline void cleanup_after_query()
  {
    if (deferred_events)
      deferred_events->rewind();
  };
  /* The general cleanup that slave applier may need at the end of session. */
  void cleanup_after_session()
  {
    if (deferred_events)
      delete deferred_events;
  };
   
  /**
    Helper function to do after statement completion.

    This function is called from an event to complete the group by
    either stepping the group position, if the "statement" is not
    inside a transaction; or increase the event position, if the
    "statement" is inside a transaction.

    @param event_log_pos
    Master log position of the event. The position is recorded in the
    relay log info and used to produce information for <code>SHOW
    SLAVE STATUS</code>.
  */
  int stmt_done(my_off_t event_log_pos);


  /**
     Set the value of a replication state flag.

     @param flag Flag to set
   */
  void set_flag(enum_state_flag flag)
  {
    m_flags |= (1UL << flag);
  }

  /**
     Get the value of a replication state flag.

     @param flag Flag to get value of

     @return @c true if the flag was set, @c false otherwise.
   */
  bool get_flag(enum_state_flag flag)
  {
    return m_flags & (1UL << flag);
  }

  /**
     Clear the value of a replication state flag.

     @param flag Flag to clear
   */
  void clear_flag(enum_state_flag flag)
  {
    m_flags &= ~(1UL << flag);
  }

  /**
     Is the replication inside a group?

     Replication is inside a group if either:
     - The OPTION_BEGIN flag is set, meaning we're inside a transaction
     - The RLI_IN_STMT flag is set, meaning we're inside a statement
     - There is an GTID owned by the thd, meaning we've passed a SET GTID_NEXT

     @retval true Replication thread is currently inside a group
     @retval false Replication thread is currently not inside a group
   */
  bool is_in_group() const {
    return (info_thd->variables.option_bits & OPTION_BEGIN) ||
      (m_flags & (1UL << IN_STMT)) ||
      (info_thd->variables.gtid_next.type == GTID_GROUP) ||
      (info_thd->variables.gtid_next.type == ANONYMOUS_GROUP) ||
      /* If a SET GTID_NEXT was issued we are inside of a group */
      info_thd->owned_gtid.sidno;
  }

  int count_relay_log_space();

  int rli_init_info();
  void end_info();
  int flush_info(bool force= FALSE);
  int flush_current_log();
  void set_master_info(Master_info *info);

  inline ulonglong get_future_event_relay_log_pos() { return future_event_relay_log_pos; }
  inline void set_future_event_relay_log_pos(ulonglong log_pos)
  {
    future_event_relay_log_pos= log_pos;
  }

  inline const char* get_group_master_log_name() { return group_master_log_name; }
  inline ulonglong get_group_master_log_pos() { return group_master_log_pos; }
  inline void set_group_master_log_name(const char *log_file_name)
  {
     strmake(group_master_log_name,log_file_name, sizeof(group_master_log_name)-1);
  }
  inline void set_group_master_log_pos(ulonglong log_pos)
  {
    group_master_log_pos= log_pos;
  }

  inline const char* get_group_relay_log_name() { return group_relay_log_name; }
  inline ulonglong get_group_relay_log_pos() { return group_relay_log_pos; }
  inline void set_group_relay_log_name(const char *log_file_name)
  {
     strmake(group_relay_log_name,log_file_name, sizeof(group_relay_log_name)-1);
  }
  inline void set_group_relay_log_name(const char *log_file_name, size_t len)
  {
     strmake(group_relay_log_name, log_file_name, len);
  }
  inline void set_group_relay_log_pos(ulonglong log_pos)
  {
    group_relay_log_pos= log_pos;
  }

  inline const char* get_event_relay_log_name() { return event_relay_log_name; }
  inline ulonglong get_event_relay_log_pos() { return event_relay_log_pos; }
  inline void set_event_relay_log_name(const char *log_file_name)
  {
     strmake(event_relay_log_name,log_file_name, sizeof(event_relay_log_name)-1);
  }
  inline void set_event_relay_log_name(const char *log_file_name, size_t len)
  {
     strmake(event_relay_log_name,log_file_name, len);
  }
  inline void set_event_relay_log_pos(ulonglong log_pos)
  {
    event_relay_log_pos= log_pos;
  }
  inline const char* get_rpl_log_name() const
  {
    return (group_master_log_name[0] ? group_master_log_name : "FIRST");
  }

#if MYSQL_VERSION_ID < 40100
  inline ulonglong get_future_master_log_pos() { return future_master_log_pos; }
#else
  inline ulonglong get_future_group_master_log_pos() { return future_group_master_log_pos; }
  inline void set_future_group_master_log_pos(ulonglong log_pos)
  {
    future_group_master_log_pos= log_pos;
  }
#endif

  static size_t get_number_info_rli_fields();

  /**
    Indicate that a delay starts.

    This does not actually sleep; it only sets the state of this
    Relay_log_info object to delaying so that the correct state can be
    reported by SHOW SLAVE STATUS and SHOW PROCESSLIST.

    Requires rli->data_lock.

    @param delay_end The time when the delay shall end.
  */
  void start_sql_delay(time_t delay_end)
  {
    mysql_mutex_assert_owner(&data_lock);
    sql_delay_end= delay_end;
    THD_STAGE_INFO(info_thd, stage_sql_thd_waiting_until_delay);
  }

  int32 get_sql_delay() { return sql_delay; }
  void set_sql_delay(time_t _sql_delay) { sql_delay= _sql_delay; }
  time_t get_sql_delay_end() { return sql_delay_end; }

  Relay_log_info(bool is_slave_recovery
#ifdef HAVE_PSI_INTERFACE
                 ,PSI_mutex_key *param_key_info_run_lock,
                 PSI_mutex_key *param_key_info_data_lock,
                 PSI_mutex_key *param_key_info_sleep_lock,
                 PSI_mutex_key *param_key_info_thd_lock,
                 PSI_mutex_key *param_key_info_data_cond,
                 PSI_mutex_key *param_key_info_start_cond,
                 PSI_mutex_key *param_key_info_stop_cond,
                 PSI_mutex_key *param_key_info_sleep_cond
#endif
                 , uint param_id
                );
  virtual ~Relay_log_info();

  /*
    Determines if a warning message on unsafe execution was
    already printed out to avoid clutering the error log
    with several warning messages.
  */
  bool reported_unsafe_warning;

  /*
    'sql_thread_kill_accepted is set to TRUE when killed status is recognized.
  */
  bool sql_thread_kill_accepted;

  time_t get_row_stmt_start_timestamp()
  {
    return row_stmt_start_timestamp;
  }

  time_t set_row_stmt_start_timestamp()
  {
    if (row_stmt_start_timestamp == 0)
      row_stmt_start_timestamp= my_time(0);

    return row_stmt_start_timestamp;
  }

  void reset_row_stmt_start_timestamp()
  {
    row_stmt_start_timestamp= 0;
  }

  void set_long_find_row_note_printed()
  {
    long_find_row_note_printed= true;
  }

  void unset_long_find_row_note_printed()
  {
    long_find_row_note_printed= false;
  }

  bool is_long_find_row_note_printed()
  {
    return long_find_row_note_printed;
  }

public:
  /**
    Delete the existing event and set a new one.  This class is
    responsible for freeing the event, the caller should not do that.
  */
  virtual void set_rli_description_event(Format_description_log_event *fdle);

  /**
    Return the current Format_description_log_event.
  */
  Format_description_log_event *get_rli_description_event() const
  {
    return rli_description_event;
  }

#if defined(HAVE_REPLICATION) && !defined(MYSQL_CLIENT)
  Commit_order_manager *get_commit_order_manager()
  {
    return commit_order_mngr;
  }

  void set_commit_order_manager(Commit_order_manager *mngr)
  {
    commit_order_mngr= mngr;
  }
#endif

  virtual bool get_skip_unique_check()
  {
    return skip_unique_check;
  }

  /**
    adaptation for the slave applier to specific master versions.
  */
  void adapt_to_master_version(Format_description_log_event *fdle);
  uchar slave_version_split[3]; // bytes of the slave server version
  /*
    relay log info repository should be updated on relay log
    rotate. But when the transaction is split across two relay logs,
    update the repository will cause unexpected results and should
    be postponed till the 'commit' of the transaction is executed.

    A flag that set to 'true' when this type of 'forced flush'(at the
    time of rotate relay log) is postponed due to transaction split
    across the relay logs.
  */
  bool force_flush_postponed_due_to_split_trans;

protected:
  Format_description_log_event *rli_description_event;

private:
  /*
    Commit order manager to order commits made by its workers. In context of
    Multi Source Replication each worker will be ordered by the coresponding
    corrdinator's order manager.
   */
  Commit_order_manager* commit_order_mngr;

  /**
    Delay slave SQL thread by this amount, compared to master (in
    seconds). This is set with CHANGE MASTER TO MASTER_DELAY=X.

    Guarded by data_lock.  Initialized by the client thread executing
    START SLAVE.  Written by client threads executing CHANGE MASTER TO
    MASTER_DELAY=X.  Read by SQL thread and by client threads
    executing SHOW SLAVE STATUS.  Note: must not be written while the
    slave SQL thread is running, since the SQL thread reads it without
    a lock when executing flush_info().
  */
  int sql_delay;

  /**
    During a delay, specifies the point in time when the delay ends.

    This is used for the SQL_Remaining_Delay column in SHOW SLAVE STATUS.

    Guarded by data_lock. Written by the sql thread.  Read by client
    threads executing SHOW SLAVE STATUS.
  */
  time_t sql_delay_end;

  uint32 m_flags;

  /*
    Before the MASTER_DELAY parameter was added (WL#344), relay_log.info
    had 4 lines. Now it has 5 lines.
  */
  static const int LINES_IN_RELAY_LOG_INFO_WITH_DELAY= 5;

  /*
    Before the WL#5599, relay_log.info had 5 lines. Now it has 6 lines.
  */
  static const int LINES_IN_RELAY_LOG_INFO_WITH_WORKERS= 6;

  /*
    Before the Id was added (BUG#2334346), relay_log.info
    had 6 lines. Now it has 7 lines.
  */
  static const int LINES_IN_RELAY_LOG_INFO_WITH_ID= 7;

  bool read_info(Rpl_info_handler *from);
  bool write_info(Rpl_info_handler *to);

  Relay_log_info(const Relay_log_info& info);
  Relay_log_info& operator=(const Relay_log_info& info);

  /*
    Runtime state for printing a note when slave is taking
    too long while processing a row event.
   */
  time_t row_stmt_start_timestamp;
  bool long_find_row_note_printed;

  std::unordered_set<std::string> rbr_column_type_mismatch_whitelist;

public:
  // store value to propagate to handler in open_tables
  bool skip_unique_check;
  /*
    Set of tables for which slave-exec-mode is considered IDEMPOTENT.
    This is modified only during sql thread startup. This set is read by
    sql threads.
  */
  std::unordered_set<std::string> rbr_idempotent_tables;
  virtual bool is_table_idempotent(const std::string &table) const
  {
    return rbr_idempotent_tables.find(table) !=
           rbr_idempotent_tables.end();
  }

  /* @see opt_slave_check_before_image_consistency */
  ulong check_before_image_consistency= 0;
  /* counter for the number of inconsistencies found */
  std::atomic<ulong> before_image_inconsistencies{0};

  virtual const std::unordered_set<std::string>*
        get_rbr_column_type_mismatch_whitelist() const
  {
    return &rbr_column_type_mismatch_whitelist;
  }

  void set_rbr_column_type_mismatch_whitelist(
      const std::unordered_set<std::string> &cols)
  {
    rbr_column_type_mismatch_whitelist= cols;
  }

#if defined(HAVE_REPLICATION) && !defined(MYSQL_CLIENT)
  /* Related to dependency tracking */

  /* Cached global variables */
  ulong mts_dependency_replication= 0;
  ulonglong mts_dependency_size= 0;
  double mts_dependency_refill_threshold= 0;
  ulonglong mts_dependency_max_keys= 0;
  my_bool mts_dependency_order_commits= TRUE;

  std::deque<std::shared_ptr<Log_event_wrapper>> dep_queue;
  mysql_mutex_t dep_lock;

  /* Mapping from key to penultimate (for multi event trx)/end event of the
     last trx that updated that table */
  std::unordered_map<Dependency_key, std::shared_ptr<Log_event_wrapper>>
                                            dep_key_lookup;
  mysql_mutex_t dep_key_lookup_mutex;

  /* Set of keys accessed by the group */
  std::unordered_set<Dependency_key> keys_accessed_by_group;

  /* Set of all DBs accessed by the current group */
  std::unordered_set<std::string> dbs_accessed_by_group;

  // Mutex-condition pair to notify when queue is/is not full
  mysql_cond_t dep_full_cond;
  bool dep_full= false;

  // Mutex-condition pair to notify when queue is/is not empty
  mysql_cond_t dep_empty_cond;
  ulonglong num_workers_waiting= 0;

  std::shared_ptr<Log_event_wrapper> prev_event;
  std::unordered_map<ulonglong, Table_map_log_event *> table_map_events;
  std::shared_ptr<Log_event_wrapper> current_begin_event;
  bool trx_queued= false;
  bool dep_sync_group= false;

  // Used to signal when a dependency worker dies
  std::atomic<bool> dependency_worker_error{false};

  mysql_cond_t dep_trx_all_done_cond;
  ulonglong num_in_flight_trx= 0;
  ulonglong num_events_in_current_group= 0;

  // Statistics
  std::atomic<ulonglong> begin_event_waits{0};
  std::atomic<ulonglong> next_event_waits{0};

  bool enqueue_dep(
      const std::shared_ptr<Log_event_wrapper> &begin_event)
  {
    mysql_mutex_assert_owner(&dep_lock);
    dep_queue.push_back(begin_event);
    return true;
  }

  std::shared_ptr<Log_event_wrapper> dequeue_dep()
  {
    mysql_mutex_assert_owner(&dep_lock);
    if (dep_queue.empty()) { return nullptr; }
    auto ret= dep_queue.front();
    dep_queue.pop_front();
    return ret;
  }

  void cleanup_group(std::shared_ptr<Log_event_wrapper> begin_event)
  {
    // Delete all events manually in bottom-up manner to avoid stack overflow
    // from cascading shared_ptr deletions
    std::stack<std::weak_ptr<Log_event_wrapper>> events;
    auto& event= begin_event;
    while (event)
    {
      events.push(event);
      event= event->next_ev;
    }

    while (!events.empty())
    {
      const auto sptr= events.top().lock();
      if (likely(sptr))
        sptr->next_ev.reset();
      events.pop();
    }
  }

  void clear_dep(bool need_dep_lock= true)
  {
    if (need_dep_lock)
      mysql_mutex_lock(&dep_lock);

    DBUG_ASSERT(num_in_flight_trx >= dep_queue.size());
    num_in_flight_trx -= dep_queue.size();
    for (const auto& begin_event : dep_queue)
      cleanup_group(begin_event);
    dep_queue.clear();

    prev_event.reset();
    current_begin_event.reset();
    table_map_events.clear();

    keys_accessed_by_group.clear();
    dbs_accessed_by_group.clear();

    mysql_cond_broadcast(&dep_empty_cond);
    mysql_cond_broadcast(&dep_full_cond);
    mysql_cond_broadcast(&dep_trx_all_done_cond);

    dep_full= false;

    mysql_mutex_lock(&dep_key_lookup_mutex);
    dep_key_lookup.clear();
    mysql_mutex_unlock(&dep_key_lookup_mutex);

    trx_queued= false;
    num_events_in_current_group= 0;

    if (need_dep_lock)
      mysql_mutex_unlock(&dep_lock);
  }
#endif // HAVE_REPLICATION and !MYSQL_CLIENT
};

bool mysql_show_relaylog_events(THD* thd);

/**
   @param  thd a reference to THD
   @return TRUE if thd belongs to a Worker thread and FALSE otherwise.
*/
inline bool is_mts_worker(const THD *thd)
{
  return thd->system_thread == SYSTEM_THREAD_SLAVE_WORKER;
}

#endif /* RPL_RLI_H */
