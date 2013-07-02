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

#include <stdlib.h>
#include <sys/time.h>
#include "dagman_metrics.h"
#include "debug.h"
#include "safe_fopen.h"
#include "condor_version.h"
#include "condor_string.h" // for getline()
#include "MyString.h"
#include "condor_arglist.h"

double DagmanMetrics::_startTime = 0.0;
MyString DagmanMetrics::_dagmanId = "";
MyString DagmanMetrics::_parentDagmanId = "";

//---------------------------------------------------------------------------
void
DagmanMetrics::SetStartTime()
{
	_startTime = GetTime();
}

//---------------------------------------------------------------------------
void
DagmanMetrics::SetDagmanIds( const char *scheddAddr,
			const CondorID &DAGManJobId, int parentDagmanCluster )
{
	MyString schedd = scheddAddr;
	schedd.replaceString( "<", "" );
	schedd.replaceString( ">", "" );

	_dagmanId = schedd + ";";
	_dagmanId += DAGManJobId._cluster;

	if ( parentDagmanCluster >= 0 ) {
		_parentDagmanId = schedd + ";";
		_parentDagmanId += parentDagmanCluster;
	}
}

//---------------------------------------------------------------------------
DagmanMetrics::DagmanMetrics( /*const*/ Dag *dag,
			const char *primaryDagFile, int rescueDagNum ) :
	_sendMetrics( false ),
	_simpleNodes( 0 ),
	_subdagNodes( 0 ),
	_simpleNodesSuccessful( 0 ),
	_simpleNodesFailed( 0 ),
	_subdagNodesSuccessful( 0 ),
	_subdagNodesFailed( 0 ), 
	_totalNodeJobTime( 0.0 )
{
	_primaryDagFile = strnewp(primaryDagFile);

	_rescueDagNum = rescueDagNum;

		//
		// Figure out whether to actually report the metrics.  We only
		// send metrics if it's enabled with the PEGASUS_METRICS
		// environment variable.  But that can be overridden by the
		// CONDOR_DEVELOPERS config macro.
		//
	const char *tmp = getenv( "PEGASUS_METRICS" );
	if ( tmp && ( ( strcasecmp( tmp, "true" ) == 0 ) ||
				( strcasecmp( tmp, "1" ) == 0 ) ) ) {
		_sendMetrics = true;
	}

	tmp = param( "CONDOR_DEVELOPERS" );
	if ( tmp && strcmp( tmp, "NONE" ) == 0 ) {
		_sendMetrics = false;
	}

		//
		// Set the metrics file name.
		//
	_metricsFile = primaryDagFile;
	_metricsFile += ".metrics";

		//
		// If we're actually sending metrics, Pegasus should have
		// created a braindump.txt file that includes a bunch of
		// Pegasus information.
		//
	if ( _sendMetrics ) {
		ParseBraindumpFile();
	}

		//
		// Get DAG node counts.
		// Note:  We don't check for nodes already marked being done (e.g.,
		// in a rescue DAG) because they should have already been reported
		// as being run.  wenger 2013-06-27
		//
	Job *node;
	dag->_jobs.Rewind();
	while ( (node = dag->_jobs.Next()) ) {
		if ( node->GetDagFile() ) {
			_subdagNodes++;
		} else {
			_simpleNodes++;
		}
	}
}

//---------------------------------------------------------------------------
DagmanMetrics::~DagmanMetrics()
{
	delete[] _primaryDagFile;
}

//---------------------------------------------------------------------------
void
DagmanMetrics::NodeFinished( bool isSubdag, bool successful )
{
	if ( isSubdag ) {
		if ( successful ) {
			_subdagNodesSuccessful++;
		} else {
			_subdagNodesFailed++;
		}
	} else {
		if ( successful ) {
			_simpleNodesSuccessful++;
		} else {
			_simpleNodesFailed++;
		}
	}
}

//---------------------------------------------------------------------------
bool
DagmanMetrics::Report( int exitCode, Dag::dag_status status )
{
	if ( !WriteMetricsFile( exitCode, status ) ) {
		return false;
	}

//TEMPTEMP -- exe needs to be full path!
	if ( _sendMetrics ) {
		const char* exe = param("DAGMAN_PEGASUS_REPORT_METRICS");	
		if(!exe) {
			exe = "condor_dagman_metrics_reporter";
			exe = "/scratch/wenger/condor_build/git2/CONDOR_SRC/personal_condor/release_dir/bin/condor_dagman_metrics_reporter";//TEMPTEMP!!!!!!
		}

		MyString metricsOutputFile( _primaryDagFile );
		metricsOutputFile += ".metrics.out";

		debug_printf( DEBUG_NORMAL,
					"Reporting metrics to Pegasus metrics server(s); output is in %s.\n",
					metricsOutputFile.Value() );

//TEMPTEMP -- probably do output/error redirect here instead of having the metrics program open its own output file -- that loses stuff that libcurl just prints...
		ArgList args;
		args.AppendArg(exe);	
		args.AppendArg("-f");
		args.AppendArg(_metricsFile.Value());
		//TEMPTEMP -- instead of passing status here, I think we should just pass whether we want the reporter to sleep -- avoids "magic constant" in reporter code
		args.AppendArg("-s");
		args.AppendArg(MyString(status));
		args.AppendArg("-o");
		args.AppendArg(metricsOutputFile.Value());

			// Dump the args to the dagman.out file
		MyString cmd; // for debug output
		args.GetArgsStringForDisplay( &cmd );
		debug_printf( DEBUG_NORMAL, "Running command <%s>\n", cmd.Value() );

		daemonCore->Create_Process(exe,args);
	} else {
		debug_printf( DEBUG_NORMAL, "Metrics not sent because of PEGASUS_METRICS or CONDOR_DEVELOPERS setting.\n" );
	}

	return true;
}

//---------------------------------------------------------------------------
bool
DagmanMetrics::WriteMetricsFile( int exitCode, Dag::dag_status status )
{
	double endTime = GetTime();
	double duration = endTime - _startTime;

	//TEMPTEMP -- hmm -- do we have to do the 'create a tmp file and move' thing?

	//TEMPTEMP -- is this the right open function?
	FILE *fp = safe_fopen_wrapper_follow( _metricsFile.Value(), "w" );
	if ( !fp ) {
		debug_printf( DEBUG_QUIET, "Could not open %s for writing.\n",
					_metricsFile.Value() );
		return false;
	}

	fprintf( fp, "{\n" );
	fprintf( fp, "    \"client\":\"%s\",\n", _plannerName.Value() );
	fprintf( fp, "    \"version\":\"%s\",\n", _plannerVersion.Value() );
	fprintf( fp, "    \"type\":\"metrics\",\n" );
	fprintf( fp, "    \"wf_uuid\":\"%s\",\n", _workflowId.Value() );
	fprintf( fp, "    \"root_wf_uuid\":\"%s\",\n", _rootWorkflowId.Value() );
	fprintf( fp, "    \"start_time\":%.3lf,\n", _startTime );
	fprintf( fp, "    \"end_time\":%.3lf,\n", endTime );
	fprintf( fp, "    \"duration\":%.3lf,\n", duration );
	fprintf( fp, "    \"exitcode\":%d,\n", exitCode );
		//TEMPTEMP -- I think we just want something like "8.1.0" here...
	MyString cv = "8.1.0";//TEMPTEMP
	fprintf( fp, "    \"dagman_version\":\"%s\",\n", cv.Value() );
	fprintf( fp, "    \"dagman_id\":\"%s\",\n", _dagmanId.Value() );
	fprintf( fp, "    \"parent_dagman_id\":\"%s\",\n",
				_parentDagmanId.Value() );
	fprintf( fp, "    \"rescue_dag_number\":%d,\n", _rescueDagNum );
	fprintf( fp, "    \"jobs\":%d,\n", _simpleNodes );
	fprintf( fp, "    \"jobs_failed\":%d,\n", _simpleNodesFailed );
	fprintf( fp, "    \"jobs_succeeded\":%d,\n", _simpleNodesSuccessful );
	fprintf( fp, "    \"dag_jobs\":%d,\n", _subdagNodes );
	fprintf( fp, "    \"dag_jobs_failed\":%d,\n", _subdagNodesFailed );
	fprintf( fp, "    \"dag_jobs_succeeded\":%d,\n", _subdagNodesSuccessful );
	fprintf( fp, "    \"total_jobs\":%d,\n", _simpleNodes + _subdagNodes );
	int totalNodesRun = _simpleNodesSuccessful + _simpleNodesFailed +
				_subdagNodesSuccessful + _subdagNodesFailed;
	fprintf( fp, "    \"total_jobs_run\":%d,\n", totalNodesRun );
	_totalNodeJobTime = 123.456;//TEMPTEMP
	fprintf( fp, "    \"total_job_time\":%.3lf,\n", _totalNodeJobTime );

		// Last item must NOT have trailing comma!
	fprintf( fp, "    \"dag_status\":%d\n", status );
	fprintf( fp, "}\n" );

	//TEMPTEMP -- check return value?
	fclose( fp );

	debug_printf( DEBUG_NORMAL, "Wrote metrics file %s.\n",
				_metricsFile.Value() );

	return true;
}

//---------------------------------------------------------------------------
double
DagmanMetrics::GetTime()
{
	double result = 0.0;

	struct timeval tv;
	if ( gettimeofday( &tv, NULL ) != 0 ) {
		debug_printf( DEBUG_QUIET,
					"Warning: gettimeofday() failed (%d, %s)\n",
					errno, strerror( errno ) );
	} else {
		result = ((double)tv.tv_sec) + (tv.tv_usec * 1.0e-6);
	}

	return result;
}

//---------------------------------------------------------------------------
void
DagmanMetrics::ParseBraindumpFile()
{
	const char *filename = getenv( "PEGASUS_BRAINDUMP_FILE" );
	if ( !filename ) {
		filename = "braindump.txt";
	}

	FILE *fp = safe_fopen_wrapper_follow( filename, "r" );
	if ( !fp ) {
		debug_printf( DEBUG_QUIET,
					"Warning:  could not open Pegasus braindump file %s\n",
					filename );
		check_warning_strictness( DAG_STRICT_2 );
		return;
	}

	const char *line;
		// Note:  getline() frees memory from the previous call each time.
	while ( (line = getline( fp ) ) ) {
		MyString lineStr( line );
		lineStr.Tokenize();
		const char *token1;
		token1 = lineStr.GetNextToken( " \t", true );
		if ( token1 ) {
			const char *token2 = lineStr.GetNextToken( " \t", true );
			if ( token2 ) {
				if ( strcmp( token1, "wf_uuid" ) == 0 ) {
					_workflowId = token2;
				} else if ( strcmp( token1, "root_wf_uuid" ) == 0 ) {
					_rootWorkflowId = token2;
				} else if ( strcmp( token1, "planner" ) == 0 ) {
					_plannerName = token2;
				} else if ( strcmp( token1, "planner_version" ) == 0 ) {
					_plannerVersion = token2;
				}
			} else {
				debug_printf( DEBUG_QUIET,
							"Warning:  no value for %s in braindump file\n",
							token1 );
				check_warning_strictness( DAG_STRICT_2 );
			}
		}
	}

	//TEMPTEMP --check return value
	fclose( fp );
}