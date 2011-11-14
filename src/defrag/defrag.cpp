/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"

#include "condor_debug.h"
#include "condor_fix_assert.h"
#include "condor_io.h"
#include "condor_constants.h"
#include "condor_classad.h"
#include "condor_attributes.h"
#include "condor_daemon_core.h"
#include "condor_config.h"
#include "condor_query.h"
#include "defrag.h"
#include "util_lib_proto.h"
#include "dc_startd.h"

char const * const ATTR_LAST_POLL = "LastPoll";

Defrag::Defrag():
	m_polling_interval(-1),
	m_polling_timer(-1),
	m_draining_per_hour(0),
	m_draining_per_poll(0),
	m_draining_per_poll_hour(0),
	m_draining_per_poll_day(0),
	m_max_draining(-1),
	m_max_whole_machines(-1),
	m_draining_schedule(DRAIN_GRACEFUL),
	m_last_poll(0)
{
}

Defrag::~Defrag()
{
	stop();
}

void Defrag::init()
{
	config();
}

void Defrag::config()
{

	ASSERT( param(m_state_file,"DEFRAG_STATE_FILE") );
	if( m_last_poll==0 ) {
		loadState();
	}

	int old_polling_interval = m_polling_interval;
	m_polling_interval = param_integer("DEFRAG_INTERVAL",600);
	if( m_polling_interval <= 0 ) {
		dprintf(D_ALWAYS,
				"DEFRAG_INTERVAL=%d, so no pool defragmentation "
				"will be done.\n", m_polling_interval);
		if( m_polling_timer != -1 ) {
			daemonCore->Cancel_Timer(m_polling_timer);
			m_polling_timer = -1;
		}
	}
	else if( m_polling_timer >= 0 ) {
		if( old_polling_interval != m_polling_interval ) {
			daemonCore->Reset_Timer(
				m_polling_timer,
				m_polling_interval,
				m_polling_interval);
		}
	}
	else {
		time_t now = time(NULL);
		int first_time = 0;
		if( m_last_poll != 0 && now-m_last_poll < m_polling_interval && m_last_poll <= now ) {
			first_time = m_polling_interval - (now-m_last_poll);
		}
		m_polling_timer = daemonCore->Register_Timer(
			first_time,
			m_polling_interval,
			(TimerHandlercpp)&Defrag::poll,
			"Defrag::poll",
			this );
	}
	if( old_polling_interval != m_polling_interval && m_polling_interval > 0 )
	{
		dprintf(D_ALWAYS,
				"Will evaluate defragmentation policy every DEFRAG_INTERVAL="
				"%d seconds.\n", m_polling_interval);
	}

	m_draining_per_hour = param_double("DEFRAG_DRAINING_MACHINES_PER_HOUR",0,0);

	double rate = m_draining_per_hour/3600.0*m_polling_interval;
	m_draining_per_poll = (int)floor(rate + 0.00001);
	if( m_draining_per_poll < 0 ) m_draining_per_poll = 0;

	double error_per_hour = (rate - m_draining_per_poll)/m_polling_interval*3600.0;
	m_draining_per_poll_hour = (int)floor(error_per_hour + 0.00001);
	if( m_draining_per_hour < 0 || m_polling_interval > 3600 ) {
		m_draining_per_hour = 0;
	}

	double error_per_day = (error_per_hour - m_draining_per_poll_hour)*24.0;
	m_draining_per_poll_day = (int)floor(error_per_day + 0.5);
	if( m_draining_per_poll_day < 0 || m_polling_interval > 3600*24 ) {
		m_draining_per_poll_day = 0;
	}
	dprintf(D_ALWAYS,"polling interval %ds, DEFRAG_DRAINING_MACHINES_PER_HOUR = %f/hour = %d/interval + %d/hour + %d/day\n",
			m_polling_interval,m_draining_per_hour,m_draining_per_poll,
			m_draining_per_poll_hour,m_draining_per_poll_day);

	m_max_draining = param_integer("DEFRAG_MAX_CONCURRENT_DRAINING",-1,-1);
	m_max_whole_machines = param_integer("DEFRAG_MAX_WHOLE_MACHINES",-1,-1);

	ASSERT( param(m_defrag_requirements,"DEFRAG_REQUIREMENTS") );
	validateExpr( m_defrag_requirements.c_str(), "DEFRAG_REQUIREMENTS" );

	ASSERT( param(m_whole_machine_expr,"DEFRAG_WHOLE_MACHINE_EXPR") );
	validateExpr( m_whole_machine_expr.c_str(), "DEFRAG_WHOLE_MACHINE_EXPR" );

	ASSERT( param(m_draining_schedule_str,"DEFRAG_DRAINING_SCHEDULE") );
	if( m_draining_schedule_str.empty() ) {
		m_draining_schedule = DRAIN_GRACEFUL;
		m_draining_schedule_str = "graceful";
	}
	else {
		m_draining_schedule = getDrainingScheduleNum(m_draining_schedule_str.c_str());
		if( m_draining_schedule < 0 ) {
			EXCEPT("Invalid draining schedule: %s\n",m_draining_schedule_str.c_str());
		}
	}

	MyString rank;
	param(rank,"DEFRAG_RANK");
	if( rank.IsEmpty() ) {
		m_rank_ad.Delete(ATTR_RANK);
	}
	else {
		if( !m_rank_ad.AssignExpr(ATTR_RANK,rank.Value()) ) {
			EXCEPT("Invalid expression for DEFRAG_RANK: %s\n",
				   rank.Value());
		}
	}
}

void Defrag::stop()
{
	if( m_polling_timer != -1 ) {
		daemonCore->Cancel_Timer(m_polling_timer);
		m_polling_timer = -1;
	}
}

static int StartdSortFunc(ClassAd *ad1,ClassAd *ad2,void *data)
{
	ClassAd *rank_ad = (ClassAd *)data;

	float rank1 = 0;
	float rank2 = 0;
	rank_ad->EvalFloat(ATTR_RANK,ad1,rank1);
	rank_ad->EvalFloat(ATTR_RANK,ad2,rank2);

	return rank1 > rank2;
}

void Defrag::validateExpr(char const *constraint,char const *constraint_source)
{
	ExprTree *requirements = NULL;

	if( ParseClassAdRvalExpr( constraint, requirements )!=0 || requirements==NULL )
	{
		EXCEPT("Invalid expression for %s: %s\n",
			   constraint_source,constraint);
	}
	delete requirements;
}

bool Defrag::queryMachines(char const *constraint,char const *constraint_source,ClassAdList &startdAds)
{
	CondorQuery startdQuery(STARTD_AD);

	validateExpr(constraint,constraint_source);
	startdQuery.addANDConstraint(constraint);

	CollectorList* collects = daemonCore->getCollectorList();
	ASSERT( collects );

	QueryResult result;
	result = collects->query(startdQuery,startdAds);
	if( result != Q_OK ) {
		dprintf(D_ALWAYS,
				"Couldn't fetch startd ads using constraint "
				"%s=%s: %s\n",
				constraint_source,constraint, getStrQueryResult(result));
		return false;
	}

	dprintf(D_FULLDEBUG,"Got %d startd ads matching %s=%s\n",
			startdAds.MyLength(), constraint_source, constraint);

	return true;
}

int Defrag::countMachines(char const *constraint,char const *constraint_source,	MachineSet *machines)
{
	ClassAdList startdAds;
	int count = 0;

	if( !queryMachines(constraint,constraint_source,startdAds) ) {
		return -1;
	}

	MachineSet my_machines;
	if( !machines ) {
		machines = &my_machines;
	}

	startdAds.Open();
	ClassAd *startd_ad;
	while( (startd_ad=startdAds.Next()) ) {
		std::string machine;
		std::string name;
		startd_ad->LookupString(ATTR_NAME,name);
		slotNameToDaemonName(name,machine);

		if( machines->count(machine) ) {
			continue;
		}

		machines->insert(machine);
		count++;
	}
	startdAds.Close();

	dprintf(D_FULLDEBUG,"Counted %d machines matching %s=%s\n",
			count,constraint_source,constraint);
	return count;
}

void Defrag::saveState()
{
	ClassAd ad;
	ad.Assign(ATTR_LAST_POLL,(int)m_last_poll);

	std::string new_state_file;
	sprintf(new_state_file,"%s.new",m_state_file.c_str());
	FILE *fp;
	if( !(fp = safe_fopen_wrapper_follow(new_state_file.c_str(), "w")) ) {
		EXCEPT("failed to save state to %s\n",new_state_file.c_str());
	}
	else {
		ad.fPrint(fp);
		fclose( fp );
		if( rotate_file(new_state_file.c_str(),m_state_file.c_str())!=0 ) {
			EXCEPT("failed to save state to %s\n",m_state_file.c_str());
		}
	}
}

void Defrag::loadState()
{
	FILE *fp;
	if( !(fp = safe_fopen_wrapper_follow(m_state_file.c_str(), "r")) ) {
		if( errno == ENOENT ) {
			dprintf(D_ALWAYS,"State file %s does not yet exist.\n",m_state_file.c_str());
		}
		else {
			EXCEPT("failed to load state from %s\n",m_state_file.c_str());
		}
	}
	else {
		int isEOF=0, errorReadingAd=0, adEmpty=0;
		ClassAd *ad = new ClassAd(fp, "...", isEOF, errorReadingAd, adEmpty);
		fclose( fp );

		if( errorReadingAd ) {
			dprintf(D_ALWAYS,"WARNING: failed to parse state from %s\n",m_state_file.c_str());
		}

		int timestamp = (int)m_last_poll;
		ad->LookupInteger(ATTR_LAST_POLL,timestamp);
		m_last_poll = (time_t)timestamp;

		dprintf(D_ALWAYS,"Last poll: %d\n",(int)m_last_poll);

		delete ad;
	}
}

void Defrag::slotNameToDaemonName(std::string const &name,std::string &machine)
{
	size_t pos = name.find('@');
	if( pos == std::string::npos ) {
		machine = name;
	}
	else {
		machine.append(name,pos+1,name.size()-pos-1);
	}
}

// n is a number per period.  If we are partly through
// the interval, make n be in proportion to how much
// is left.
static int prorate(int n,int time_remaining,int period,int granularity)
{
	double frac = ((double)time_remaining)/period;

		// Add in maximum time in this interval that could have been
		// missed due to polling interval (granularity).

	frac += ((double)granularity)/period;

	int answer = (int)floor(n*frac + 0.5);

	if( (n > 0 && answer > n) || (n < 0 && answer < n) ) {
		return n; // never exceed magnitude of n
	}
	if( answer*n < 0 ) { // never flip sign
		return 0;
	}
	return answer;
}

void Defrag::poll()
{
	dprintf(D_FULLDEBUG,"Evaluating defragmentation policy.\n");

		// If we crash during this polling cycle, we will have saved
		// the time of the last poll, so the next cycle will be
		// scheduled on the false assumption that a cycle ran now.  In
		// this way, we error on the side of draining too little
		// rather than too much.

	time_t now = time(NULL);
	time_t prev = m_last_poll;
	m_last_poll = now;
	saveState();

	if( (int)ceil(m_draining_per_hour) <= 0 ) {
		dprintf(D_ALWAYS,"Doing nothing, because DEFRAG_DRAINING_MACHINES_PER_HOUR=%f\n",
				m_draining_per_hour);
		return;
	}

	if( m_max_draining == 0 ) {
		dprintf(D_ALWAYS,"Doing nothing, because DEFRAG_MAX_CONCURRENT_DRAINING=0\n");
		return;
	}

	if( m_max_whole_machines == 0 ) {
		dprintf(D_ALWAYS,"Doing nothing, because DEFRAG_MAX_WHOLE_MACHINES=0\n");
		return;
	}

	int num_to_drain = m_draining_per_poll;

	time_t last_hour    = (prev / 3600)*3600;
	time_t current_hour = (now  / 3600)*3600;
	time_t last_day     = (prev / (3600*24))*3600*24;
	time_t current_day  = (now  / (3600*24))*3600*24;

	if( current_hour != last_hour ) {
		num_to_drain += prorate(m_draining_per_poll_hour,now-current_hour,3600,m_polling_interval);
	}
	if( current_day != last_day ) {
		num_to_drain += prorate(m_draining_per_poll_day,now-current_day,3600*24,m_polling_interval);
	}
	if( num_to_drain <= 0 ) {
		dprintf(D_ALWAYS,"Doing nothing, because draining rate has already been achieved.\n");
		return;
	}

	char const *draining_constraint = "Draining && Offline=!=True";
	int num_draining = countMachines(draining_constraint,"<internal draining constraint>");

	MachineSet whole_machines;
	int num_whole_machines = countMachines(m_whole_machine_expr.c_str(),"DEFRAG_WHOLE_MACHINE_EXPR",&whole_machines);

	dprintf(D_ALWAYS,"There are currently %d draining and %d whole machines.\n",
			num_draining,num_whole_machines);

	if( m_max_draining >= 0 ) {
		if( num_draining >= m_max_draining ) {
			dprintf(D_ALWAYS,"Doing nothing, because DEFRAG_MAX_CONCURRENT_DRAINING=%d and there are %d draining machines.\n",
					m_max_draining, num_draining);
			return;
		}
		else if( num_draining < 0 ) {
			dprintf(D_ALWAYS,"Doing nothing, because DEFRAG_MAX_CONCURRENT_DRAINING=%d and the query to count draining machines failed.\n",
					m_max_draining);
			return;
		}
	}

	if( m_max_whole_machines >= 0 ) {
		if( num_whole_machines >= m_max_whole_machines ) {
			dprintf(D_ALWAYS,"Doing nothing, because DEFRAG_MAX_WHOLE_MACHINES=%d and there are %d whole machines.\n",
					m_max_whole_machines, num_whole_machines);
			return;
		}
	}

		// Even if m_max_whole_machines is -1 (infinite), we still need
		// the list of whole machines in order to filter them out in
		// the draining selection algorithm, so abort now if the
		// whole machine query failed.
	if( num_whole_machines < 0 ) {
		dprintf(D_ALWAYS,"Doing nothing, because the query to find whole machines failed.\n");
		return;
	}

	dprintf(D_ALWAYS,"Looking for %d machines to drain.\n",num_to_drain);

	ClassAdList startdAds;
	std::string requirements;
	sprintf(requirements,"(%s) && Draining =!= true",m_defrag_requirements.c_str());
	if( !queryMachines(requirements.c_str(),"DEFRAG_REQUIREMENTS",startdAds) ) {
		dprintf(D_ALWAYS,"Doing nothing, because the query to select machines matching DEFRAG_REQUIREMENTS failed.\n");
		return;
	}

	startdAds.Shuffle();
	startdAds.Sort(StartdSortFunc,&m_rank_ad);

	startdAds.Open();
	int num_drained = 0;
	ClassAd *startd_ad;
	MachineSet machines_done;
	while( (startd_ad=startdAds.Next()) ) {
		std::string machine;
		std::string name;
		startd_ad->LookupString(ATTR_NAME,name);
		slotNameToDaemonName(name,machine);

		if( machines_done.count(machine) ) {
			dprintf(D_FULLDEBUG,
					"Skipping %s: already attempted to drain %s in this cycle.\n",
					name.c_str(),machine.c_str());
			continue;
		}

		if( whole_machines.count(machine) ) {
			dprintf(D_FULLDEBUG,
					"Skipping %s: because it is already running as a whole machine.\n",
					name.c_str());
			continue;
		}

		if( drain(startd_ad) ) {
			machines_done.insert(machine);

			if( ++num_drained >= num_to_drain ) {
				dprintf(D_ALWAYS,
						"Drained maximum number of machines allowed in this cycle (%d).\n",
						num_to_drain);
				break;
			}
		}
	}
	startdAds.Close();

	dprintf(D_ALWAYS,"Drained %d machines (wanted to drain %d machines).\n",
			num_drained,num_drained);

	dprintf(D_FULLDEBUG,"Done evaluating defragmentation policy.\n");
}

bool
Defrag::drain(ClassAd *startd_ad)
{
	ASSERT( startd_ad );

	std::string name;
	startd_ad->LookupString(ATTR_NAME,name);

	dprintf(D_ALWAYS,"Initiating %s draining of %s.\n",
			m_draining_schedule_str.c_str(),name.c_str());

	DCStartd startd( startd_ad );

	int graceful_completion = 0;
	startd_ad->LookupInteger(ATTR_EXPECTED_MACHINE_GRACEFUL_DRAINING_COMPLETION,graceful_completion);
	int quick_completion = 0;
	startd_ad->LookupInteger(ATTR_EXPECTED_MACHINE_QUICK_DRAINING_COMPLETION,quick_completion);
	int graceful_badput = 0;
	startd_ad->LookupInteger(ATTR_EXPECTED_MACHINE_GRACEFUL_DRAINING_BADPUT,graceful_badput);
	int quick_badput = 0;
	startd_ad->LookupInteger(ATTR_EXPECTED_MACHINE_QUICK_DRAINING_BADPUT,quick_badput);

	time_t now = time(NULL);
	std::string draining_check_expr;
	double badput_growth_tolerance = 1.25; // for now, this is hard-coded
	int negligible_badput = 1200;
	int negligible_deadline_slippage = 1200;
	if( m_draining_schedule <= DRAIN_GRACEFUL ) {
		dprintf(D_ALWAYS,"Expected draining completion time is %ds; expected draining badput is %d cpu-seconds\n",
				(int)(graceful_completion-now),graceful_badput);
		sprintf(draining_check_expr,"%s <= %d && %s <= %d",
				ATTR_EXPECTED_MACHINE_GRACEFUL_DRAINING_COMPLETION,
				graceful_completion + negligible_deadline_slippage,
				ATTR_EXPECTED_MACHINE_GRACEFUL_DRAINING_BADPUT,
				(int)(badput_growth_tolerance*graceful_badput) + negligible_badput);
	}
	else { // DRAIN_FAST and DRAIN_QUICK are effectively equivalent here
		dprintf(D_ALWAYS,"Expected draining completion time is %ds; expected draining badput is %d cpu-seconds\n",
				(int)(quick_completion-now),quick_badput);
		sprintf(draining_check_expr,"%s <= %d && %s <= %d",
				ATTR_EXPECTED_MACHINE_QUICK_DRAINING_COMPLETION,
				quick_completion + negligible_deadline_slippage,
				ATTR_EXPECTED_MACHINE_QUICK_DRAINING_BADPUT,
				(int)(badput_growth_tolerance*quick_badput) + negligible_badput);
	}

	std::string request_id;
	bool resume_on_completion = true;
	bool rval = startd.drainJobs( m_draining_schedule, resume_on_completion, draining_check_expr.c_str(), request_id );
	if( !rval ) {
		dprintf(D_ALWAYS,"Failed to send request to drain %s\n",startd.name());
		return false;
	}

	return true;
}