// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AutomationTool;
using Gauntlet;
using EpicGames.Core;
using Log = Gauntlet.Log;
using UnrealBuildBase;	// for Unreal.RootDirectory
using UnrealBuildTool;	// for UnrealTargetPlatform

using static AutomationTool.CommandUtils;
using UE;
using System.Configuration;
using System.Reflection;

namespace AutomatedPerfTest
{
	public interface IAutomatedPerfTest
	{
		List<string> GetTestsFromConfig();
	}

	/// <summary>
	/// Implementation of a Gauntlet TestNode for AutomatedPerfTest plugin
	/// </summary>
	/// <typeparam name="TConfigClass"></typeparam>
	public abstract class AutomatedPerfTestNode<TConfigClass> : UnrealTestNode<TConfigClass>
		where TConfigClass : AutomatedPerfTestConfigBase, new()
	{
		public string SummaryTable = "default";
		public AutomatedPerfTestNode(UnrealTestContext InContext) : base(InContext)
		{
			// We need to save off the build name as if this is a preflight that suffix will be stripped
			// after GetConfiguration is called. This will cause a mismatch in CreateReport.
			OriginalBuildName = Globals.Params.ParseValue("BuildName", InContext.BuildInfo.BuildName);
			Log.Info("Setting OriginalBuildName to {OriginalBuildName}", OriginalBuildName);
			
			TestGuid = Guid.NewGuid();
			Log.Info("Your Test GUID is :\n" + TestGuid.ToString() + '\n');

			InitHandledErrors();

			LogParser = null;
		}

		public override bool StartTest(int Pass, int InNumPasses)
		{
			LogParser = null;
			return base.StartTest(Pass, InNumPasses);
		}

		public class HandledError
		{
			public string ClientErrorString;
			public string GauntletErrorString;

			/// <summary>
			/// String name for the log category that should be used to filter errors. Defaults to null, i.e. no filter.
			/// </summary>
			public string CategoryName;

			// If error is verbose, will output debugging information such as state
			public bool Verbose;

			public HandledError(string ClientError, string GauntletError, string Category, bool VerboseIn = false)
			{
				ClientErrorString = ClientError;
				GauntletErrorString = GauntletError;
				CategoryName = Category;
				Verbose = VerboseIn;
			}
		}

		/// <summary>
		/// List of errors with special-cased gauntlet messages.
		/// </summary>
		public List<HandledError> HandledErrors { get; set; }

		/// <summary>
		/// Guid associated with each test run for ease of differentiation between different runs on same build.
		/// </summary>
		public Guid TestGuid { get; protected set; }

		/// <summary>
		/// Base artifact output path for current instance of this test node.
		/// </summary>
		public string BaseOutputPath { get; protected set; }

		/// <summary>
		/// Returns the APT Performance Artifact Path. 
		/// </summary>
		/// <param name="Platform"></param>
		/// <returns>{ProjectName}/Saved/Performance/{SubTest}/{Platform}</returns>
		public string GetPerformanceReportArtifactOutputPath(UnrealTargetPlatform Platform)
		{ 
			if(BaseOutputPath == null)
			{
				return TempPerfCSVDir.FullName;
			}

			return Path.Combine(BaseOutputPath, GetSubtestName(GetCachedConfiguration()), Platform.ToString());
		}

		/// <summary>
		/// Track client log messages that have been written to the test logs.
		/// </summary>
		private UnrealLogStreamParser LogParser;

		/// <summary>
		// Temporary directory for perf report CSVs
		/// </summary>
		private DirectoryInfo TempPerfCSVDir => new DirectoryInfo(Path.Combine(Unreal.RootDirectory.FullName, "GauntletTemp", "PerfReportCSVs"));

		/// <summary>
		// Holds the build name as is, since if this is a preflight the suffix will be stripped after GetConfiguration is called.
		/// </summary>
		private string OriginalBuildName = null;

		/// <summary>
		/// If true, local reports will be generated at the end of the perf test.
		/// </summary>
		private bool GenerateLocalReport = false;

		/// <summary>
		/// If true, the resulting CSV files will be imported via Perf Report Server
		/// importer. 
		/// </summary>
		private bool GeneratePRSReport = false;

		/// <summary>
		/// Set up the base list of possible expected errors, plus the messages to deliver if encountered.
		/// </summary>
		protected virtual void InitHandledErrors()
		{
			HandledErrors = new List<HandledError>();
		}

		protected virtual string GetNormalizedInsightsFileName(string FileName) => FileName.Replace(".csv", ".utrace");

		protected string GetNormalizedInsightsFileName(string CSVFileName, string TestTypeLiteral)
		{
			int Index = CSVFileName.IndexOf(TestTypeLiteral);

			if (Index < 0)
			{
				Index = CSVFileName.ToLower().IndexOf(".csv");
			}
			else
			{
				Index = Index + TestTypeLiteral.Length;
			}

			return CSVFileName.Substring(0, Index) + ".utrace";
		}

		/// <summary>
		/// Periodically called while test is running. Updates logs.
		/// </summary>
		public override void TickTest()
		{
			IAppInstance App = null;

			if (TestInstance.ClientApps == null)
			{
				App = TestInstance.ServerApp;
			}
			else
			{
				if (TestInstance.ClientApps.Length > 0)
				{
					App = TestInstance.ClientApps.First();
				}
			}

			if (App != null)
			{
				if (LogParser == null)
				{
					LogParser = new UnrealLogStreamParser(App.GetLogBufferReader());
				}
				LogParser.ReadStream();
				string LogChannelName = Context.BuildInfo.ProjectName + "Test";
				List<string> TestLines = LogParser.GetLogFromChannel(LogChannelName, false).ToList();

				string LogCategory = "Log" + LogChannelName;
				string LogCategoryError = LogCategory + ": Error:";
				string LogCategoryWarning = LogCategory + ": Warning:";
				
				foreach (string Line in TestLines)
				{
					if (Line.StartsWith(LogCategoryError))
					{
						ReportError(Line);
					}
					else if (Line.StartsWith(LogCategoryWarning))
					{
						ReportWarning(Line);
					}
					else
					{
						Log.Info(Line);
					}
				}
			}

			base.TickTest();
		}

		/// <summary>
		/// This allows using a per-branch config to ignore certain issues
		/// that were inherited from Main and will be addressed there
		/// </summary>
		/// <param name="InArtifacts"></param>
		/// <returns></returns>
		protected override UnrealLog CreateLogSummaryFromArtifact(UnrealRoleArtifacts InArtifacts)
		{
			UnrealLog LogSummary = base.CreateLogSummaryFromArtifact(InArtifacts);

			IgnoredIssueConfig IgnoredIssues = new IgnoredIssueConfig();

			string IgnoredIssuePath = GetCachedConfiguration().IgnoredIssuesConfigAbsPath;

			if (!File.Exists(IgnoredIssuePath))
			{
				Log.Info("No IgnoredIssue Config found at {0}", IgnoredIssuePath);
			}
			else if (IgnoredIssues.LoadFromFile(IgnoredIssuePath))
			{
				Log.Info("Loaded IgnoredIssue config from {0}", IgnoredIssuePath);

				IEnumerable<UnrealLog.CallstackMessage> IgnoredEnsures = LogSummary.Ensures.Where(E => IgnoredIssues.IsEnsureIgnored(this.Name, E.Message));
				IEnumerable<UnrealLog.LogEntry> IgnoredWarnings = LogSummary.LogEntries.Where(E => E.Level == UnrealLog.LogLevel.Warning && IgnoredIssues.IsWarningIgnored(this.Name, E.Message));
				IEnumerable<UnrealLog.LogEntry> IgnoredErrors = LogSummary.LogEntries.Where(E => E.Level == UnrealLog.LogLevel.Error && IgnoredIssues.IsErrorIgnored(this.Name, E.Message));

				if (IgnoredEnsures.Any())
				{
					Log.Info("Ignoring {0} ensures.", IgnoredEnsures.Count());
					Log.Info("\t{0}", string.Join("\n\t", IgnoredEnsures.Select(E => E.Message)));
					LogSummary.Ensures = LogSummary.Ensures.Except(IgnoredEnsures).ToArray();
				}
				if (IgnoredWarnings.Any())
				{
					Log.Info("Ignoring {0} warnings.", IgnoredWarnings.Count());
					Log.Info("\t{0}", string.Join("\n\t", IgnoredWarnings.Select(E => E.Message)));
					LogSummary.LogEntries = LogSummary.LogEntries.Except(IgnoredWarnings).ToArray();
				}
				if (IgnoredErrors.Any())
				{
					Log.Info("Ignoring {0} errors.", IgnoredErrors.Count());
					Log.Info("\t{0}", string.Join("\n\t", IgnoredErrors.Select(E => E.Message)));
					LogSummary.LogEntries = LogSummary.LogEntries.Except(IgnoredErrors).ToArray();
				}
			}


			return LogSummary;
		}

		protected override UnrealProcessResult GetExitCodeAndReason(StopReason InReason, UnrealLog InLogSummary, UnrealRoleArtifacts InArtifacts, out string ExitReason, out int ExitCode)
		{
			// Check for login failure
			UnrealLogParser Parser = new UnrealLogParser(InArtifacts.AppInstance.GetLogReader());
			TConfigClass Config = GetCachedConfiguration();

			ExitReason = "";
			ExitCode = -1;

			foreach (HandledError ErrorToCheck in HandledErrors)
			{
				string[] MatchingErrors = Parser.GetErrors(ErrorToCheck.CategoryName).Where(E => E.Contains(ErrorToCheck.ClientErrorString)).ToArray();
				if (MatchingErrors.Length > 0)
				{
					ExitReason = string.Format("Test Error: {0} {1}", ErrorToCheck.GauntletErrorString, ErrorToCheck.Verbose ? "\"" + MatchingErrors[0] + "\"" : "");
					ExitCode = -1;
					return UnrealProcessResult.TestFailure;
				}
			}

			// If this is a Test Target Configuration and we have configured to ignore logging,
			// we can check the process exit code and return from here. This is especially useful
			// when logging is disabled in Test builds, but we still want to exit the test
			// successfully. 
			UnrealTargetConfiguration TargetConfig = InArtifacts.SessionRole.Configuration;
			bool bIgnoreLoggingInTest = Config.IgnoreTestBuildLogging && TargetConfig == UnrealTargetConfiguration.Test && !InLogSummary.EngineInitialized;
			bool bTestHasErrorLog = InLogSummary.FatalError != null || InLogSummary.HasTestExitCode;

			// This is a major assumption that Gauntlet captures the process exit code on all
			// platforms and is the only thing indicating if the test has actually passed or
			// failed in the absence of logging. 
			int ProcessExitCode = InArtifacts.AppInstance.ExitCode;
			if (InReason == StopReason.Completed && bIgnoreLoggingInTest && ProcessExitCode == 0)
			{
				ExitCode = 0;
				ExitReason = "Test build exited successfully without logs.";
				return UnrealProcessResult.ExitOk;
			}
			else if (InReason == StopReason.Completed && bIgnoreLoggingInTest && ProcessExitCode != 0 && !bTestHasErrorLog)
			{
				// Process has not exited cleanly and we do not have any error log messages. 
				// Fail the test here and provide context to user.
				ExitCode = ProcessExitCode;
				ExitReason = "Test build exited with error. Please enable logging in build for more information.";
				return UnrealProcessResult.TestFailure;
			}

			// Let the user know that if their tests are failing but process exit code is 0
			// and they have logging disabled in Test builds that they can ignore logging
			// failures if they wish to do so. 
			if(!InLogSummary.EngineInitialized && TargetConfig == UnrealTargetConfiguration.Test && ProcessExitCode == 0)
			{
				Log.Warning("*** Engine Initialization log not detected in Test build with Exit Code 0. " +
					"This test will fail. " +
					"Try passing `-AutomatedPerfTest.IgnoreTestBuildLogging` while running this test " +
					"or pass `-set:APTIgnoreTestBuildLogging=true` if running via BuildGraph if you " +
					"wish to ignore log parsing checks in this test or recompile with logging enabled. ***");
			}

			return base.GetExitCodeAndReason(InReason, InLogSummary, InArtifacts, out ExitReason, out ExitCode);
		}

		public override ITestReport CreateReport(TestResult Result, UnrealTestContext Context, UnrealBuildSource Build, IEnumerable<UnrealRoleResult> Artifacts, string ArtifactPath)
		{
			if (Result == TestResult.Passed)
			{
				UnrealTargetPlatform Platform = Context.GetRoleContext(UnrealTargetRole.Client).Platform;
				TConfigClass Config = GetCachedConfiguration();
				string OutputPath = GetPerformanceReportArtifactOutputPath(Platform);

				if (Config.DoInsightsTrace)
				{
					CopyInsightsTraceToOutput(ArtifactPath, OutputPath);
				}
				
				if (GetCurrentPass() <= GetNumPasses() && Config.DoCSVProfiler)
				{
					// Our artifacts from each iteration such as the client log will be overwritten by subsequent iterations so we need to copy them out to another dir
					// to preserve them until we're ready to make our report on the final iteration.
					CopyPerfFilesToOutputDir(ArtifactPath, OutputPath);

					bool bGeneratedLocalReport = false;
					bool bGeneratedPRSReport = false;

					// Local report generation is useful for people conducting tests locally without a centralized server (for A/B testing for instance).
					// To make it work out of the box, enable by default for non-build machine runs
					if (GenerateLocalReport)
					{
						// NOTE: This does not currently work with long paths due to the CsvTools not properly supporting them.
						Log.Info("Generating local performance reports using PerfReportTool.");
						bGeneratedLocalReport = GenerateLocalPerfReport(Platform, OutputPath);
					}

					// On build machines, default to producing PRS report
					if (GeneratePRSReport)
					{
						Dictionary<string, dynamic> CommonDataSourceFields = new Dictionary<string, dynamic>
						{
							{"HordeJobUrl", Globals.Params.ParseValue("JobDetails", null)}
						};
						
						Log.Info("Creating perf server importer with build name {BuildName}", OriginalBuildName);
						string DataSourceName = GetConfiguration().DataSourceName;
						string ImportDirOverride = Globals.Params.ParseValue("PerfReportServerImportDir", null);
						Log.Info("Creating PRS Importer for data source '{0}' and import dir override (if any) '{1}'.",
							DataSourceName, ImportDirOverride);
						ICsvImporter Importer = ReportGenUtils.CreatePerfReportServerImporter(DataSourceName, OriginalBuildName,
							IsBuildMachine, ImportDirOverride, CommonDataSourceFields);
						if (Importer != null)
						{
							// Recursively grab all the csv files we copied to the temp dir and convert them to binary.
							List<FileInfo> AllBinaryCsvFiles = ReportGenUtils.CollectAndConvertCsvFilesToBinary(OutputPath);
							if (AllBinaryCsvFiles.Count == 0)
							{
								throw new AutomationException($"No Csv files found in {OutputPath}");
							}

							// The corresponding log for each csv sits in the same subdirectory as the csv file itself.
							IEnumerable<CsvImportEntry> ImportEntries = AllBinaryCsvFiles
								.Select(CsvFile => new CsvImportEntry(CsvFile.FullName, Path.Combine(CsvFile.Directory.FullName, "ClientOutput.log")));

							// todo update this so it associates videos with the correct CSVs
							IEnumerable<CsvImportEntry> CsvImportEntries = ImportEntries as CsvImportEntry[] ?? ImportEntries.ToArray();
							if (GetConfiguration().DoInsightsTrace)
							{
								string InsightsFilename = Path.GetFileNameWithoutExtension(CsvImportEntries.First().CsvFilename);

								InsightsFilename = GetNormalizedInsightsFileName(InsightsFilename);

								// recursively look for trace files that match the CSV's filename in the artifact path
								string[] MatchingTraces = FindFiles($"*{InsightsFilename}", true, ArtifactPath);
								if(MatchingTraces.Length > 0)
								{
									if (MatchingTraces.Length > 1)
									{
										Log.Warning("Multiple Insights traces were found in {ArtifactPath} matching pattern *{InsightsFilename}. Only the first will be attached to the CSV import for this test.",
											ArtifactPath, InsightsFilename);										
									}
									CsvImportEntries.First().AddAdditionalFile("Insights", MatchingTraces.First());
								}
								else
								{
									Log.Warning("Insights was requested, but no matching insights traces were found  matching pattern *{InsightsFilename} in {ArtifactPath}",
										InsightsFilename, ArtifactPath);
								}
							}

							if (GetConfiguration().DoVideoCapture)
							{
								string VideoPath = Path.Combine(ArtifactPath, "Client", "Videos");
								string[] VideoFiles = Directory.GetFiles(VideoPath, "*.mp4");
								if (VideoFiles.Length > 0)
								{
									foreach (var VideoFile in VideoFiles)
									{
										CsvImportEntries.First().AddAdditionalFile("Video", Path.Combine(VideoPath, VideoFile));
									}
								}
								else
								{
									Log.Warning("Video capture was requested, but no videos were found in path {VideoPath}", VideoPath);
								}
							}

							// Create the import batch
							Importer.Import(CsvImportEntries);
							// trust blindly for now...
							bGeneratedPRSReport = true;
						}
						else
						{
							Log.Warning("Unable to create PRS Importer.");
						}

						// Cleanup the temp dir
						if(TempPerfCSVDir.Exists)
						{
							TempPerfCSVDir.Delete(recursive: true);
						}
					}

					if (!bGeneratedLocalReport && !bGeneratedPRSReport)
					{
						Log.Warning("Did not generate neither local nor a PRS report.");
					}
				}
			}
			else
			{
				Log.Warning("Skipping performance report generation because the perf report test failed.");
			}

			return base.CreateReport(Result, Context, Build, Artifacts, ArtifactPath);
		}

		/// <summary>
		/// Get report type for current configuration
		/// </summary>
		/// <param name="ReportType"></param>
		/// <param name="SummaryTableType"></param>
		/// <param name="HistoricalReportType"></param>
		private void GetReportType(out string ReportType, out string SummaryTableType, out string HistoricalReportType)
		{
			ReportType = "ClientPerf";
			SummaryTableType = SummaryTable;
			HistoricalReportType = "autoPerfReportStandard";

			if (GetCachedConfiguration().DoLLM)
			{
				ReportType = "LLM";
				SummaryTableType = "autoPerfReportLlm";
				HistoricalReportType = "autoPerfReportLlm";
			}
		}

		/// <summary>
		/// Produces a detailed csv report using PerfReportTool.
		/// Also, stores perf data in the perf cache, and generates a historic report using the data the cache contains.
		/// </summary>
		private bool GenerateLocalPerfReport(UnrealTargetPlatform Platform, string OutputPath)
		{
			string perfreportTool = "";
			perfreportTool = Path.Combine("PerfreportTool.dll");
	
			var ToolPath = FileReference.Combine(Unreal.EngineDirectory, "Binaries", "DotNET", "CsvTools", perfreportTool);
			if (!FileReference.Exists(ToolPath))
			{
				Log.Error("Failed to find perf report utility at this path: \"{ToolPath}\".", ToolPath);
				return false;
			}

			var ReportConfigDir = GetCachedConfiguration().ReportConfigDir;
			if (string.IsNullOrEmpty(ReportConfigDir))
			{
				// default to the report types and graphs provided by APT 
				ReportConfigDir = Path.Combine(Unreal.EngineDirectory.ToString(), "Plugins", "Performance", "AutomatedPerfTesting", "Build", "Scripts", "PerfReport");	
			}

			var ReportPath = GetCachedConfiguration().ReportPath;
			if(string.IsNullOrEmpty(ReportPath))
			{
				ReportPath = Path.Combine(OutputPath, "Reports");
			}

			string ReportCacheDir = Path.Combine(OutputPath, "Cache");

			// Csv files may have been output in one of two places.
			// Check both...
			var CsvsPaths = new[]
			{
				Path.Combine(OutputPath, "CSV")
			};

			var DiscoveredCsvs = new List<string>();
			foreach (var CsvsPath in CsvsPaths)
			{
				if (Directory.Exists(CsvsPath))
				{
					DiscoveredCsvs.AddRange(
						from CsvFile in Directory.GetFiles(CsvsPath, "*.csv", SearchOption.AllDirectories)
						select CsvFile);
				}
			}

			if (DiscoveredCsvs.Count == 0)
			{
				Log.Error("Test completed successfully but no csv profiling results were found. Searched paths were:\r\n  {Paths}", string.Join("\r\n  ", CsvsPaths.Select(s => $"\"{s}\"")));
				return false;
			}

			// Find the newest csv file and get its directory
			// (PerfReportTool will only output cached data in -csvdir mode)
			var NewestFile =
				(from CsvFile in DiscoveredCsvs
				 let Timestamp = File.GetCreationTimeUtc(CsvFile)
				 orderby Timestamp descending
				 select CsvFile).First();
			var NewestDir = Path.GetDirectoryName(NewestFile);

			Log.Info("Using perf report cache directory \"{ReportCacheDir}\".", ReportCacheDir);
			Log.Info("Using perf report output directory \"{ReportPath}\".", ReportPath);
			Log.Info("Using csv results directory \"{NewestDir}\". Generating historic perf report data...", NewestDir);

			// Make sure the cache and output directories exist
			if (!Directory.Exists(ReportCacheDir))
			{
				try { Directory.CreateDirectory(ReportCacheDir); }
				catch (Exception Ex)
				{
					Log.Error("Failed to create perf report cache directory \"{ReportCacheDir}\". {Ex}", ReportCacheDir, Ex);
					return false;
				}
			}
			if (!Directory.Exists(ReportPath))
			{
				try { Directory.CreateDirectory(ReportPath); }
				catch (Exception Ex)
				{
					Log.Error("Failed to create perf report output directory \"{ReportPath}\". {Ex}", ReportPath, Ex);
					return false;
				}
			}

			// Win64 is actually called "Windows" in csv profiles
			var PlatformNameFilter = Platform == UnrealTargetPlatform.Win64 ? "Windows" : $"{Platform}";

			string SearchPattern = $"{Context.BuildInfo.ProjectName}*";
			string ReportType = null;
			string SummaryTableType = null;
			string HistoricalSummaryType = null;

			GetReportType(out ReportType, out SummaryTableType, out HistoricalSummaryType);

			// Produce the detailed report, and update the perf cache
			var DetailedReportPath = Path.Combine(ReportPath, "Detailed");
			string[] CsvGenerationArgs = new[]
			{
				 $"{ToolPath.FullName}",
				 $"-csvdir \"{NewestDir}\"",
				 $"-reportType \"{ReportType}\"",
				 $"-o \"{DetailedReportPath}\"",
				 $"-reportxmlbasedir \"{ReportConfigDir}\"",
				 $"-summaryTable {SummaryTableType}",
				 $"-summaryTableCache \"{ReportCacheDir}\"",
				 $"-metadatafilter platform=\"{PlatformNameFilter}\""
			};

			string PerfReportToolArgs = string.Join(' ', CsvGenerationArgs);
			RunAndLog(CmdEnv, CmdEnv.DotnetMsbuildPath, PerfReportToolArgs, out int ErrorCode);
			if (ErrorCode != 0)
			{
				Log.Error("PerfReportTool returned error code \"{ErrorCode}\" while generating detailed report.", ErrorCode);
			}

			// Now generate the all-time historic summary report
			HistoricReport("HistoricReport_AllTime", new[]
			{
				$"platform={PlatformNameFilter}"
			});

			// 14 days historic report
			HistoricReport($"HistoricReport_14Days", new[]
			{
				$"platform={PlatformNameFilter}",
				$"starttimestamp>={DateTimeOffset.Now.ToUnixTimeSeconds() - (14 * 60L * 60L * 24L)}"
			});

			// 7 days historic report
			HistoricReport($"HistoricReport_7Days", new[]
			{
				$"platform={PlatformNameFilter}",
				$"starttimestamp>={DateTimeOffset.Now.ToUnixTimeSeconds() - (7 * 60L * 60L * 24L)}"
			});

			void HistoricReport(string Name, IEnumerable<string> Filter)
			{
				var Args = new[]
				{
					$"{ToolPath.FullName}",
					$"-reportType \"{ReportType}\"",
					$"-summarytablecachein \"{ReportCacheDir}\"",
					$"-summaryTableFilename \"{Name}.html\"",
					$"-reportxmlbasedir \"{ReportConfigDir}\"",
					$"-o \"{ReportPath}\"",
					$"-metadatafilter \"{string.Join(" and ", Filter)}\"",
					$"-summaryTable {HistoricalSummaryType}",
					$"-condensedSummaryTable {HistoricalSummaryType}",
					$"-reportLinkRootPath \"{DetailedReportPath}\\\"",
					"-emailtable",
					"-recurse"
				};

				var ArgStr = string.Join(" ", Args);

				RunAndLog(CmdEnv, CmdEnv.DotnetMsbuildPath, ArgStr, out ErrorCode);
				if (ErrorCode != 0)
				{
					Log.Error("PerfReportTool returned error code \"{ErrorCode}\" while generating historic report.", ErrorCode);
				}
			}

			return true;
		}

		private void CopyPerfFilesToOutputDir(string FromArtifactPath, string ToOutputDir = null)
		{
			if(string.IsNullOrEmpty(ToOutputDir))
			{
				// Fallback path
				ToOutputDir = TempPerfCSVDir.FullName;
			}

			DirectoryInfo OutputDirectory = new DirectoryInfo(ToOutputDir);
			if (!OutputDirectory.Exists)
			{
				Log.Info("Creating temp perf csv dir: {OutputDirectory}", OutputDirectory);
				OutputDirectory.Create();
			}

			DirectoryInfo CSVDirectory = new DirectoryInfo(Path.Combine(ToOutputDir, "CSV"));
			if (!CSVDirectory.Exists)
			{
				Log.Info($"Creating CSV Directory: {CSVDirectory}");
				CSVDirectory.Create();
			}

			string ClientArtifactDir = Path.Combine(FromArtifactPath, "Client");
			string ClientLogPath = Path.Combine(ClientArtifactDir, "ClientOutput.log");


			string CSVPath = PathUtils.FindRelevantPath(ClientArtifactDir, "Profiling", "CSV");
			if (string.IsNullOrEmpty(CSVPath))
			{
				Log.Warning("Failed to find CSV folder folder in {ClientArtifactDir}", ClientArtifactDir);
				return;
			}

			// Grab all the csv files that have valid metadata.
			// We don't want to convert to binary in place as the legacy reports require the raw csv.
			List<FileInfo> CsvFiles = ReportGenUtils.CollectValidCsvFiles(CSVPath);
			if (CsvFiles.Count > 0)
			{
				// We only want to copy the latest file as the other will have already been copied when this was run for those iterations.
				CsvFiles.SortBy(Info => Info.LastWriteTimeUtc);
				FileInfo LatestCsvFile = CsvFiles.Last();

				// Create a subdir for each pass as we want to store the csv and log together in the same dir to make it easier to find them later.
				string PassDir = Path.Combine(ToOutputDir, "Logs");
				Directory.CreateDirectory(PassDir);

				FileInfo LogFileInfo = new FileInfo(ClientLogPath);
				if (LogFileInfo.Exists)
				{
					string Guid = TestGuid.ToString();
					string DestLogFile = $"CL{Context.BuildInfo.Changelist}-Pass{GetCurrentPass()}-{Guid.Substring(0, Guid.IndexOf("-"))}-{LogFileInfo.Name}";
					string LogDestPath = Path.Combine(PassDir, DestLogFile);
					Log.Info("Copying Log {ClientLogPath} To {LogDest}", ClientLogPath, LogDestPath);
					LogFileInfo.CopyTo(LogDestPath, true);
				}
				else
				{
					Log.Warning("No log file was found at {ClientLogPath}", ClientLogPath);
				}

				string Extension = LatestCsvFile.Extension;
				string OutputCSVFile = $"{LatestCsvFile.Name.Replace(Extension, "")}-Pass{GetCurrentPass()}{Extension}";
				string CsvDestPath = Path.Combine(CSVDirectory.FullName, OutputCSVFile);
				Log.Info("Copying Csv {CsvPath} To {CsvDestPath}", LatestCsvFile.FullName, CsvDestPath);
				LatestCsvFile.CopyTo(CsvDestPath, true);
			}
			else
			{
				Log.Warning("No valid csv files found in {CSVPath}", CSVPath);
			}
		}
		
		protected virtual string GetSubtestName(TConfigClass Config)
		{
			// Options like DoLLM and DoInsightsTrace are heavy enough to be in their own subtest type
			// Since they are not exclusive, create a yet another subtest type if both are specified
			if (Config.DoLLM && Config.DoInsightsTrace)
			{
				throw new AutomationException($"Running Insights trace with LLM is not a practical test due to LLM's large overhead.");
			}

			if (Config.DoInsightsTrace)
			{
				if (Config.TraceChannels != "default,screenshot,stats")
				{
					throw new AutomationException($"Running Insights trace with non-default channels {Config.TraceChannels}, new subtest type is needed to avoid contaminating 'Insights' subtest results.");
				}

				return "Insights";
			}

			if (Config.DoLLM)
			{
				return "LLM";
			}

			if (Config.DoGPUPerf)
			{
				return "GPUPerf";
			}

			if (Config.DoGPUReshape)
			{
				return "GPUReshape";
			}

			return "Perf";
		}

		public override TConfigClass GetConfiguration()
		{
			TConfigClass Config = base.GetConfiguration();
			Config.MaxDuration = Context.TestParams.ParseValue("MaxDuration", 60 * 60);  // 1 hour max

			if(string.IsNullOrEmpty(Config.PerfOutputPath))
			{
				Config.PerfOutputPath = Path.Combine(Context.BuildInfo.ProjectPath.Directory.FullName, "Saved", "Performance");
			}

			BaseOutputPath = Path.Combine(Config.PerfOutputPath, GetType().Name); 

			UnrealTestRole ClientRole = Config.RequireRole(UnrealTargetRole.Client);
			// the controller will be added by the subclasses

			ClientRole.CommandLineParams.AddOrAppendParamValue("logcmds", "LogHttp Verbose, LogAutomatedPerfTest Verbose");
			
			ClientRole.CommandLineParams.Add("-deterministic");

			Log.Info("AutomatedPerfTestNode<>.GetConfiguration(): Config.DoFPSChart={0}, Config.DoCSVProfiler={1}, Config.DoVideoCapture={2}, Config.DoInsightsTrace={3}, Config.DoLLM={4}", 
				Config.DoFPSChart, Config.DoCSVProfiler, Config.DoVideoCapture, Config.DoInsightsTrace, Config.DoLLM);
			
			ClientRole.CommandLineParams.AddOrAppendParamValue("AutomatedPerfTest.TestID", Config.TestID);

			if (Config.DeviceProfileOverride != String.Empty)
			{
				ClientRole.CommandLineParams.AddOrAppendParamValue("AutomatedPerfTest.DeviceProfileOverride", Config.DeviceProfileOverride);
			}

			if (Config.DoInsightsTrace)
			{
				ClientRole.CommandLineParams.Add("AutomatedPerfTest.DoInsightsTrace");
				if (Config.TraceChannels != String.Empty)
				{
					ClientRole.CommandLineParams.AddOrAppendParamValue("AutomatedPerfTest.TraceChannels", Config.TraceChannels);
				}
			}

			if (Config.DoLLM)
			{
				ClientRole.CommandLineParams.Add("llm");
				ClientRole.CommandLineParams.Add("llmcsv");
			}

			if (Config.DoGPUPerf)
			{
				// see ReplayRun.py, reducedAsyncComputeCommands
				// We enable r.nanite.asyncrasterization.shadowdepths because nanite overlaps with itself, so it's possible to time accurately without distorting other timings
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.StencilLODMode 1");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.VolumetricRenderTarget.PreferAsyncCompute 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.LumenScene.Lighting.AsyncCompute 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.Lumen.DiffuseIndirect.AsyncCompute 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.Bloom.AsyncCompute 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.nanite.asyncrasterization.shadowdepths 1");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.TSR.AsyncCompute 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.RayTracing.AsyncBuild 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.DFShadowAsyncCompute 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.AmbientOcclusion.Compute 1"); // 1 here means compute on the graphics pipe
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.LocalFogVolume.TileCullingUseAsync 0");
				ClientRole.CommandLineParams.AddOrAppendParamValue("execcmds", "r.SkyAtmosphereASyncCompute 0");
			}

			if (Config.DoGPUPerf || Config.LockDynamicRes)
			{
				ClientRole.CommandLineParams.Add("AutomatedPerfTest.LockDynamicRes");
			}

			if(Config.DoFPSChart)
			{
				ClientRole.CommandLineParams.Add("AutomatedPerfTest.DoFPSChart");
			}

			if (Config.DoCSVProfiler)
			{
				ConfigureCSVProfiler(Config, ClientRole);
			}

			if (Config.DoVideoCapture)
			{
				ClientRole.CommandLineParams.Add("AutomatedPerfTest.DoVideoCapture");
			}

			return Config;
		}
		
		public void CopyInsightsTraceToOutput(string FromArtifactPath, string ToOutputPath)
		{
			Log.Info("Copying test insights trace from artifact path to report cache");
			
			// find all the available trace paths
			var DiscoveredTraces = new List<string>();
			if (Directory.Exists(FromArtifactPath))
			{
				DiscoveredTraces.AddRange(
					from TraceFile in Directory.GetFiles(FromArtifactPath, "*.utrace", SearchOption.AllDirectories)
					select TraceFile);
			}
			
			// if we couldn't find any traces, report that and bail out
			if (DiscoveredTraces.Count == 0)
			{
				Log.Error("Test completed successfully but no trace results were found. Searched path was {ArtifactPath}", FromArtifactPath);
				return;
			}
			
			// iterate over each of the discovered traces (there should be one for each test case that was run)
			// first, sort the cases by timestamp
			string[] SortedTraces =
				(from TraceFile in DiscoveredTraces
					let Timestamp = File.GetCreationTimeUtc(TraceFile)
					orderby Timestamp descending
					select TraceFile).ToArray();
			
			var ReportPath = Path.Combine(ToOutputPath, "Traces");
			if (SortedTraces.Length > 0)
			{
				string Filename = Path.GetFileNameWithoutExtension(SortedTraces[0]);
				string PerfTracePath = Path.Combine(ReportPath, Filename + ".utrace");

				Log.Info("Copying latest utrace file from {ArtifactPath} to Perf .utrace path: {PerfTracePath}", FromArtifactPath,
					PerfTracePath);
				
				// just try the copy over, and log a failure, but don't bail out of the test.
				try
				{
					InternalUtils.SafeCreateDirectory(Path.GetDirectoryName(PerfTracePath), true);
					File.Copy(SortedTraces[0], PerfTracePath);
				}
				catch (Exception e)
				{
					Log.Warning("Failed to copy local trace file: {Text}", e);
				}
			}
		}

		protected void ConfigureCSVProfiler(TConfigClass Config, UnrealTestRole ClientRole)
		{
			if (Globals.Params.ParseParam("LocalReports") || (!IsBuildMachine && !Globals.Params.ParseParam("NoLocalReports")))
			{
				GenerateLocalReport = true;
			}

			if((IsBuildMachine || Globals.Params.ParseParam("PerfReportServer")) &&
						!Globals.Params.ParseParam("SkipPerfReportServer"))
			{
				GeneratePRSReport = true;
			}

			ClientRole.CommandLineParams.Add("AutomatedPerfTest.DoCSVProfiler");
			ClientRole.CommandLineParams.Add("csvGpuStats");

			// Add CSV metadata
			List<string> CsvMetadata =
			[
				$"testname={Context.BuildInfo.ProjectName}",
				"gauntletTestType=AutomatedPerfTest",
				$"gauntletSubTest={GetSubtestName(Config)}",
				"testBuildIsPreflight=" + (ReportGenUtils.IsTestingPreflightBuild(OriginalBuildName) ? "1" : "0"),
				$"testBuildVersion={OriginalBuildName}",
				"testconfigname=" + Config.TestConfigName,
			];

			if (!string.IsNullOrEmpty(Context.BuildInfo.Branch) && Context.BuildInfo.Changelist != 0)
			{
				CsvMetadata.Add("branch=" + Context.BuildInfo.Branch);
				CsvMetadata.Add("changelist=" + Context.BuildInfo.Changelist);
			}

			if (Config.DoGPUPerf)
			{
				CsvMetadata.Add("ReducedAsyncCompute=1");
			}

			ClientRole.CommandLineParams.Add("csvMetadata", "\"" + String.Join(",", CsvMetadata) + "\"");
		}

		static protected void GetConfigValues(UnrealTestContext Context, string IniSection, string IniElement, out IReadOnlyList<string> Values)
		{
			IniConfigUtil.GetConfigHierarchy(Context, ConfigHierarchyType.Engine).TryGetValues(IniSection, IniElement, out Values);
		}

		static protected string GetPathInProject(UnrealTestContext Context, string InPath)
		{
			return Path.Combine(Context.BuildInfo.ProjectPath.Directory.FullName, InPath);
		}

		static protected void ReadConfigArray(UnrealTestContext Context, string IniSection, string IniElement, Action<string> Process)
		{
			IReadOnlyList<string> Configs = null;
			GetConfigValues(Context, IniSection, IniElement, out Configs);
			List<string> ConfigList = Configs == null ? new List<string>() : Configs.ToList();
			ConfigList.ForEach(Process);
		}
	}

	/// <summary>
	/// Implementation of a Gauntlet TestNode for AutomatedPerfTest plugin
	/// </summary>
	/// <typeparam name="TConfigClass"></typeparam>
	public abstract class AutomatedSequencePerfTestNode<TConfigClass> : AutomatedPerfTestNode<TConfigClass>, IAutomatedPerfTest
		where TConfigClass : AutomatedSequencePerfTestConfig, new()
	{
		public AutomatedSequencePerfTestNode(UnrealTestContext InContext) : base(InContext)
		{
			SummaryTable = "sequence";
		}

		public override TConfigClass GetConfiguration()
		{
			TConfigClass Config = base.GetConfiguration();

			Config.DataSourceName = Config.GetDataSourceName(Context.BuildInfo.ProjectName, "Sequence");
			
			// extend the role(s) that we initialized in the base class
			if (Config.GetRequiredRoles(UnrealTargetRole.Client).Any())
			{
				foreach(UnrealTestRole ClientRole in Config.GetRequiredRoles(UnrealTargetRole.Client))
				{
					ClientRole.Controllers.Add("AutomatedSequencePerfTest");
					
					// if a specific MapSequenceComboName was defined in the commandline to UAT, then add that to the commandline for the role
					if (!string.IsNullOrEmpty(Config.MapSequenceComboName))
					{
						// use add Unique, since there should only ever be one of these specified
						ClientRole.CommandLineParams.AddUnique($"AutomatedPerfTest.SequencePerfTest.MapSequenceName",
							Config.MapSequenceComboName);
					}
				}
			}

			return Config;
		}

		public List<string> GetTestsFromConfig()
		{
			List<string> OutSequenceList = new List<string>();
			ReadConfigArray(Context,
				"/Script/AutomatedPerfTesting.AutomatedSequencePerfTestProjectSettings",
				"MapsAndSequencesToTest",
				Config =>
				{
					Dictionary<string, string> SequenceConfig = IniConfigUtil.ParseDictionaryFromConfigString(Config);
					string ComboName;
					if (SequenceConfig.TryGetValue("ComboName", out ComboName))
					{
						OutSequenceList.Add(ComboName.Replace("\"", ""));
					}
				});

			return OutSequenceList;
		}

		protected override string GetNormalizedInsightsFileName(string CSVFileName)
		{
			return GetNormalizedInsightsFileName(CSVFileName, "_Sequence");
		}
	}

	/// <summary>
	/// Implementation of a Gauntlet TestNode for AutomatedPerfTest plugin
	/// </summary>
	/// <typeparam name="TConfigClass"></typeparam>
	public abstract class AutomatedReplayPerfTestNode<TConfigClass> : AutomatedPerfTestNode<TConfigClass>, IAutomatedPerfTest
		where TConfigClass : AutomatedReplayPerfTestConfig, new()
	{
		public AutomatedReplayPerfTestNode(UnrealTestContext InContext) : base(InContext)
		{
			Config = null;
		}

		/// <summary>
		/// Handler to explicitly copy files to the device. It is assumed this 
		/// is called when Gauntlet configures the device before starting the
		/// tests. 
		/// </summary>
		public void CopyReplayToDevice(ITargetDevice Device)
		{
			if (Config == null)
			{
				Log.Warning("ConfigureDevice() called before Test Node Configuration is set.");
				return;
			}

			// At the moment in some pathways we don't seem to copy
			// files in a role. This ensures we have a copy of the 
			// replay files on the target device regardless. 
			if (Config.GetRequiredRoles(UnrealTargetRole.Client).Any())
			{
				foreach (UnrealTestRole ClientRole in Config.GetRequiredRoles(UnrealTargetRole.Client))
				{
					UnrealTargetPlatform Platform = Context.GetRoleContext(UnrealTargetRole.Client).Platform;
					if (Device.Platform == Platform && ClientRole.FilesToCopy.Any())
					{
						Device.CopyAdditionalFiles(ClientRole.FilesToCopy);
					}
				}
			}
		}

		public override TConfigClass GetConfiguration()
		{
			Config = base.GetConfiguration();
			
			Config.DataSourceName = Config.GetDataSourceName(Context.BuildInfo.ProjectName, ReplayDataSourceName);

			// Since we do not have a device instance at this stage. These platform support
			// instances would help if we would still like to know if the target platform has
			// certain features or functionality we need to be aware of in order to pass the
			// right configuration to Gauntlet.
			IEnumerable<IPlatformTargetSupport> PlatformSupportInstances = Gauntlet.Utils.InterfaceHelpers.FindImplementations<IPlatformTargetSupport>();

			IEnumerable<UnrealTestRole> ClientRoles = Config.GetRequiredRoles(UnrealTargetRole.Client);
			// extend the role(s) that we initialized in the base class
			if (ClientRoles.Any())
			{
				foreach (UnrealTestRole ClientRole in ClientRoles)
				{
					ClientRole.Controllers.Add("AutomatedReplayPerfTest");
					ClientRole.FilesToCopy.Clear();

					UnrealTargetPlatform Platform = Context.GetRoleContext(UnrealTargetRole.Client).Platform;
					IPlatformTargetSupport MountablePlatform = PlatformSupportInstances
						.Where(TargetSupport => TargetSupport.IsHostMountingSupported() && TargetSupport.Platform == Platform)
						.FirstOrDefault();

					List<string> ReplaysFromConfig = GetTestsFromConfig();

					// Replay name not provided
					if (string.IsNullOrEmpty(Config.ReplayName))
					{
						if(ReplaysFromConfig == null || !ReplaysFromConfig.Any())
						{
							throw new AutomationException("No replays found in settings or provided via arguments.");
						}

						// Use the first replay found. We need to extract the replay path from settings as we need to 
						// post process the path depending on the target platform.
						Config.ReplayName = ReplaysFromConfig[0];
					}
					else
					{
						foreach(string Replay in ReplaysFromConfig)
						{
							if(!string.IsNullOrEmpty(Replay) && Replay.Contains(Config.ReplayName))
							{
								Log.Info("Found replay in settings");
								Config.ReplayName = Replay;
								break;
							}
						}
					}

					string ReplayName = Path.GetFullPath(Config.ReplayName);
					bool bFileExists = File.Exists(ReplayName);
					if (!bFileExists)
					{
						// In case the replay file specified is not in settings
						// or not an absolute path. Check under project directory
						// to be sure.
						ReplayName = GetPathInProject(Context, Config.ReplayName);
						bFileExists = File.Exists(ReplayName);
					}

					if(!bFileExists)
					{
						throw new AutomationException($"Replay file '{Config.ReplayName}' not found");
					}

					if (MountablePlatform != null) 
					{
						// If current device supports host mounting of files, update the path and use host mounting 
						ReplayName = MountablePlatform.GetHostMountedPath(ReplayName);
						Log.Info($"Host Mountable Platform Detected. Updated Replay File Path to: {ReplayName} ");
					}
					else
					{
						// Copy replay file to Demos folder in device if host mounting is not supported
						Log.Info($"Copy Replay File Path to Device: {ReplayName}");
						UnrealFileToCopy FileToCopy = new UnrealFileToCopy(ReplayName, EIntendedBaseCopyDirectory.Demos, Path.GetFileName(ReplayName));
						ClientRole.FilesToCopy.Add(FileToCopy);
						ClientRole.ConfigureDevice = CopyReplayToDevice;

						// If we copy to the "Demos" folder, we just need to pass the Replay Name without
						// path and extension as the replay subsystem will automatically pick up the file
						// from this folder.
						ReplayName = Path.GetFileNameWithoutExtension(ReplayName);
					}

					ClientRole.CommandLineParams.AddUnique(ReplayParamName, ReplayName);
					Log.Info($"{ReplayParamName}=\"{ReplayName}\"");
				}
			}

			return Config;
		}

		public List<string> GetTestsFromConfig()
		{
			List<string> OutReplayList = new List<string>();
			ReadConfigArray(Context,
				"/Script/AutomatedPerfTesting.AutomatedReplayPerfTestProjectSettings",
				"ReplaysToTest",
				Config =>
				{
					Dictionary<string, string> ReplayConfig = IniConfigUtil.ParseDictionaryFromConfigString(Config);
					string Path;
					if (ReplayConfig.TryGetValue("FilePath", out Path))
					{
						OutReplayList.Add(GetPathInProject(Context, Path.Replace("\"", "")));
					}
				});

			return OutReplayList;
		}
		protected override string GetNormalizedInsightsFileName(string CSVFileName)
		{
			return GetNormalizedInsightsFileName(CSVFileName, "_Replay");
		}

		private readonly string ReplayDataSourceName = "ReplayRun";
		private readonly string ReplayParamName = "AutomatedPerfTest.ReplayPerfTest.ReplayName";
		private TConfigClass Config;
	}

	/// <summary>
	/// Implementation of a Gauntlet TestNode for AutomatedPerfTest plugin
	/// </summary>
	/// <typeparam name="TConfigClass"></typeparam>
	public abstract class AutomatedStaticCameraPerfTestNode<TConfigClass> : AutomatedPerfTestNode<TConfigClass>, IAutomatedPerfTest
		where TConfigClass : AutomatedStaticCameraPerfTestConfig, new()
	{
		public AutomatedStaticCameraPerfTestNode(UnrealTestContext InContext) : base(InContext)
		{
			SummaryTable = "staticcamera";
		}

		public List<string> GetTestsFromConfig()
		{
			List<string> OutMaps = new List<string>();
			ReadConfigArray(Context,
				"/Script/AutomatedPerfTesting.AutomatedStaticCameraPerfTestProjectSettings",
				"MapsToTest", 
				Map => OutMaps.Add(Map.Replace("\"", "")));

			return OutMaps;
		}

		public override TConfigClass GetConfiguration()
		{
			TConfigClass Config = base.GetConfiguration();
			
			Config.DataSourceName = Config.GetDataSourceName(Context.BuildInfo.ProjectName, "StaticCamera");
			
			// extend the role(s) that we initialized in the base class
			if (Config.GetRequiredRoles(UnrealTargetRole.Client).Any())
			{
				foreach(UnrealTestRole ClientRole in Config.GetRequiredRoles(UnrealTargetRole.Client))
				{
					ClientRole.Controllers.Add("AutomatedPlacedStaticCameraPerfTest");
					
					// if a specific MapName was defined in the commandline to UAT, then add that to the commandline for the role
					if (!string.IsNullOrEmpty(Config.MapName))
					{
						// use add Unique, since there should only ever be one of these specified
						ClientRole.CommandLineParams.AddUnique($"AutomatedPerfTest.StaticCameraPerfTest.MapName",
							Config.MapName);
					}
				}
			}

			return Config;
		}

		protected override string GetNormalizedInsightsFileName(string CSVFileName)
		{
			return GetNormalizedInsightsFileName(CSVFileName, "_StaticCamera");
		}
	}

	/// <summary>
	/// "Standard issue" implementation usable for samples that don't need anything more advanced
	/// </summary>
	public class SequenceTest : AutomatedSequencePerfTestNode<AutomatedSequencePerfTestConfig>
	{
		public SequenceTest(Gauntlet.UnrealTestContext InContext)
			: base(InContext)
		{
		}
	}
	
	/// <summary>
	/// "Standard issue" implementation usable for samples that don't need anything more advanced
	/// </summary>
	public class StaticCameraTest : AutomatedStaticCameraPerfTestNode<AutomatedStaticCameraPerfTestConfig>
	{
		public StaticCameraTest(Gauntlet.UnrealTestContext InContext)
			: base(InContext)
		{
		}
	}

	/// <summary>
	/// Implementation of a Gauntlet TestNode for AutomatedPerfTest plugin
	/// </summary>
	/// <typeparam name="TConfigClass"></typeparam>
	public abstract class AutomatedMaterialPerfTestNode<TConfigClass> : AutomatedPerfTestNode<TConfigClass>, IAutomatedPerfTest
		where TConfigClass : AutomatedMaterialPerfTestConfig, new()
	{
		public AutomatedMaterialPerfTestNode(UnrealTestContext InContext) : base(InContext)
		{
			SummaryTable = "materials";
		}

		public List<string> GetTestsFromConfig()
		{
			List<string> OutMaterials = new List<string>();
			ReadConfigArray(Context,
				"/Script/AutomatedPerfTesting.AutomatedMaterialPerfTestProjectSettings",
				"MaterialsToTest",
				Material => OutMaterials.Add(Material.Replace("\"", "")));

			return OutMaterials;
		}

		public override TConfigClass GetConfiguration()
		{
			TConfigClass Config = base.GetConfiguration();
			
			Config.DataSourceName = Config.GetDataSourceName(Context.BuildInfo.ProjectName, "Material");
			
			// extend the role(s) that we initialized in the base class
			if (Config.GetRequiredRoles(UnrealTargetRole.Client).Any())
			{
				foreach(UnrealTestRole ClientRole in Config.GetRequiredRoles(UnrealTargetRole.Client))
				{
					ClientRole.Controllers.Add("AutomatedMaterialPerfTest");
				}
			}

			return Config;
		}

		protected override string GetNormalizedInsightsFileName(string CSVFileName)
		{
			return GetNormalizedInsightsFileName(CSVFileName, "_Materials");
		}
	}

	/// <summary>
	/// "Standard issue" implementation usable for samples that don't need anything more advanced
	/// </summary>
	public class MaterialTest : AutomatedMaterialPerfTestNode<AutomatedMaterialPerfTestConfig>
	{
		public MaterialTest(Gauntlet.UnrealTestContext InContext)
			: base(InContext)
		{
		}
	}

	/// <summary>
	/// "Standard issue" implementation usable for samples that don't need anything more advanced
	/// </summary>
	public class ReplayTest : AutomatedReplayPerfTestNode<AutomatedReplayPerfTestConfig>
	{
		public ReplayTest(Gauntlet.UnrealTestContext InContext)
			: base(InContext)
		{
		}
	}

	public class DefaultTest : AutomatedPerfTestNode<AutomatedPerfTestConfigBase>
	{
		public DefaultTest(UnrealTestContext InContext) 
			: base(InContext)
		{
			InitDefaultTestTypes();
		}

		private ITestNode CreateDefaultTestType(out Type TestNodeType)
		{
			IAutomatedPerfTest TestNode = null;
			TestNodeType = null;

			foreach (Type TestType in CandidateTestTypes)
			{
				try
				{
					ConstructorInfo Constructor = TestType.GetConstructor([typeof(UnrealTestContext)]);
					TestNode = Constructor?.Invoke([Context]) as IAutomatedPerfTest;
				}
				catch (Exception)
				{
					continue;
				}

				// Return the first candidate test type which have test(s) configured in project settings
				TestNodeType = TestType;
				List<string> Tests = TestNode.GetTestsFromConfig();
				if (Tests != null && Tests.Count > 0)
				{
					// Once we have found the first compatible test, bail out. 
					break;
				}
			}

			return TestNode as ITestNode;
		}

		public void InitDefaultTestTypes()
		{
			CandidateTestTypes = Gauntlet.Utils.InterfaceHelpers.FindTypes<IAutomatedPerfTest>(true, bConcreteTypesOnly:true).ToHashSet();
		}

		public override AutomatedPerfTestConfigBase GetConfiguration()
		{
			if (CachedConfig != null) 
			{
				return CachedConfig;
			}

			Type TestType;
			ITestNode TestNode = CreateDefaultTestType(out TestType);
			if(TestNode == null)
			{
				throw new AutomationException("Could not find a default test for given project. " +
					"Configure one of the available Automated Perf Tests in settings before re-running this test.");
			}

			dynamic TestNodeObject = Convert.ChangeType(TestNode, TestType);
			AutomatedPerfTestConfigBase Config = TestNodeObject?.GetConfiguration();
			
			// Let Default Test enable CSV profiler by default. 
			Config.DoCSVProfiler = true;
			ConfigureCSVProfiler(Config, Config.RequireRole(UnrealTargetRole.Client)); 

			// Pull all info we need from derived test node. 
			CachedConfig = Config;
			BaseOutputPath = TestNodeObject?.BaseOutputPath;
			return Config;
		}

		private HashSet<Type> CandidateTestTypes = new HashSet<Type>(); 
	}
}
