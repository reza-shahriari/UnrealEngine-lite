// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;
using Microsoft.Extensions.Logging;

namespace EpicGames.UHT.Exporters.CodeGen
{
	[UnrealHeaderTool]
	class UhtCodeGenerator
	{
		[UhtExporter(Name = "CodeGen", Description = "Standard UnrealEngine code generation", Options = UhtExporterOptions.Default,
			CppFilters = new string[] { "*.generated.cpp", "*.generated.*.cpp", "*.gen.cpp", "*.gen.*.cpp" },
			HeaderFilters = new string[] { "*.generated.h" })]
		public static void CodeGenerator(IUhtExportFactory factory)
		{
			UhtCodeGenerator generator = new(factory);
			generator.Generate();
		}

		public struct PackageInfo
		{
			public string StrippedName { get; set; }
		}
		public PackageInfo[] PackageInfos { get; set; }

		public struct HeaderInfo
		{
			public Task? Task { get; set; }
			public string IncludePath { get; set; }
			public string FileId { get; set; }
			public uint BodyHash { get; set; }
			public bool NeedsPushModelHeaders { get; set; }
			public bool NeedsFastArrayHeaders { get; set; }
			public bool NeedsVerseValue { get; set; }
			public bool NeedsVerseString { get; set; }
			public bool NeedsVerseCodeGen { get; set; }
			public bool NeedsVerseField { get; set; }
			public bool NeedsVersePointer { get; set; }
			public bool NeedsVerseTask { get; set; }
		}
		public HeaderInfo[] HeaderInfos { get; set; }

		public struct ObjectInfo
		{
			public string RegisteredSingletonName { get; set; }
			public string UnregisteredSingletonName { get; set; }
			public string RegisteredExternalDecl { get; set; }
			public string UnregisteredExternalDecl { get; set; }
			public UhtClass? NativeInterface { get; set; }
			public UhtProperty? FastArrayProperty { get; set; }
			public uint Hash { get; set; }
		}
		public ObjectInfo[] ObjectInfos { get; set; }

		public readonly IUhtExportFactory Factory;
		public UhtSession Session => Factory.Session;
		public UhtScriptStruct? FastArraySerializer { get; set; } = null;

		private UhtCodeGenerator(IUhtExportFactory factory)
		{
			Factory = factory;
			HeaderInfos = new HeaderInfo[Factory.Session.HeaderFileTypeCount];
			ObjectInfos = new ObjectInfo[Factory.Session.ObjectTypeCount];
			PackageInfos = new PackageInfo[Factory.Session.PackageTypeCount];
		}

		private void Generate()
		{
			List<Task?> prereqs = new();

			FastArraySerializer = Session.FindType(null, UhtFindOptions.SourceName | UhtFindOptions.ScriptStruct, "FFastArraySerializer") as UhtScriptStruct;

			// Perform some startup initialization to compute things we need over and over again
			
			if (Session.GoWide)
			{
				Parallel.ForEach(Factory.Session.Modules, module =>
				{
					InitModuleInfo(module);
				});
			}
			else
			{
				foreach (UhtModule module in Factory.Session.Modules)
				{
					InitModuleInfo(module);
				}
			}

			// Generate the files for the header files
			foreach (UhtHeaderFile headerFile in Session.SortedHeaderFiles)
			{
				if (headerFile.ShouldExport)
				{
					prereqs.Clear();
					foreach (UhtHeaderFile referenced in headerFile.ReferencedHeadersNoLock)
					{
						if (headerFile != referenced)
						{
							prereqs.Add(HeaderInfos[referenced.HeaderFileTypeIndex].Task);
						}
					}

					HeaderInfos[headerFile.HeaderFileTypeIndex].Task = Factory.CreateTask(prereqs,
						(IUhtExportFactory factory) =>
						{
							new UhtHeaderCodeGeneratorHFile(this, headerFile).Generate(factory);
							new UhtHeaderCodeGeneratorCppFile(this, headerFile).Generate(factory);
						});
				}
			}

			// Generate the files for the modules
			List<Task?> generatedModules = new(Session.Modules.Count);
			foreach (UhtModule module in Session.Modules)
			{
				bool writeHeader = false;
				prereqs.Clear();
				foreach (UhtHeaderFile headerFile in module.Headers)
				{
					prereqs.Add(HeaderInfos[headerFile.HeaderFileTypeIndex].Task);
					if (!writeHeader)
					{
						foreach (UhtType type in headerFile.Children)
						{
							if (type is UhtClass classObj)
							{
								if (classObj.ClassType != UhtClassType.NativeInterface &&
									classObj.ClassFlags.HasExactFlags(EClassFlags.Native | EClassFlags.Intrinsic, EClassFlags.Native) &&
									!classObj.ClassExportFlags.HasAllFlags(UhtClassExportFlags.NoExport))
								{
									writeHeader = true;
									break;
								}
							}
						}
					}
				}

				generatedModules.Add(Factory.CreateTask(prereqs,
					(IUhtExportFactory factory) =>
					{
						List<UhtHeaderFile> moduleSortedHeaders = GetSortedHeaderFiles(module);
						if (writeHeader)
						{
							new UhtPackageCodeGeneratorHFile(this, module).Generate(factory, moduleSortedHeaders);
						}
						new UhtPackageCodeGeneratorCppFile(this, module).Generate(factory, moduleSortedHeaders);
					}));
			}

			// Wait for all the packages to complete
			List<Task> moduleTasks = new(generatedModules.Count);
			foreach (Task? output in generatedModules)
			{
				if (output != null)
				{
					moduleTasks.Add(output);
				}
			}
			Task.WaitAll(moduleTasks.ToArray());
		}

		#region Utility functions
		/// <summary>
		/// Return the singleton name for an object
		/// </summary>
		/// <param name="obj">The object in question.</param>
		/// <param name="registered">If true, return the registered singleton name.  Otherwise return the unregistered.</param>
		/// <returns>Singleton name or "nullptr" if Object is null</returns>
		public string GetSingletonName(UhtObject? obj, bool registered)
		{
			if (obj == null)
			{
				return "nullptr";
			}
			return registered ? ObjectInfos[obj.ObjectTypeIndex].RegisteredSingletonName : ObjectInfos[obj.ObjectTypeIndex].UnregisteredSingletonName;
		}

		/// <summary>
		/// Return the external declaration for an object
		/// </summary>
		/// <param name="obj">The object in question.</param>
		/// <param name="registered">If true, return the registered external declaration.  Otherwise return the unregistered.</param>
		/// <returns>External declaration</returns>
		public string GetExternalDecl(UhtObject obj, bool registered)
		{
			return GetExternalDecl(obj.ObjectTypeIndex, registered);
		}

		/// <summary>
		/// Return the external declaration for an object
		/// </summary>
		/// <param name="objectIndex">The object in question.</param>
		/// <param name="registered">If true, return the registered external declaration.  Otherwise return the unregistered.</param>
		/// <returns>External declaration</returns>
		public string GetExternalDecl(int objectIndex, bool registered)
		{
			return registered ? ObjectInfos[objectIndex].RegisteredExternalDecl : ObjectInfos[objectIndex].UnregisteredExternalDecl;
		}
		#endregion

		#region Information initialization
		private void InitModuleInfo(UhtModule module)
		{
			StringBuilder builder = new();

			foreach (UhtPackage package in module.Packages)
			{
				InitPackageInfo(builder, package);
			}
			foreach (UhtHeaderFile headerFile in module.Headers)
			{
				InitHeaderInfo(builder, headerFile);
			}
		}

		private void InitPackageInfo(StringBuilder builder, UhtPackage package)
		{
			ref PackageInfo packageInfo = ref PackageInfos[package.PackageTypeIndex];
			packageInfo.StrippedName = package.SourceName.Replace('/', '_');

			// Construct the names used commonly during export
			ref ObjectInfo objectInfo = ref ObjectInfos[package.ObjectTypeIndex];
			builder.Clear();
			builder.Append("Z_Construct_UPackage_");
			builder.Append(packageInfo.StrippedName);
			objectInfo.UnregisteredSingletonName = objectInfo.RegisteredSingletonName = builder.ToString();
			objectInfo.UnregisteredExternalDecl = objectInfo.RegisteredExternalDecl = $"\tUPackage* {objectInfo.RegisteredSingletonName}();\r\n";
		}

		private void InitHeaderInfo(StringBuilder builder, UhtHeaderFile headerFile)
		{
			ref HeaderInfo headerInfo = ref HeaderInfos[headerFile.HeaderFileTypeIndex];

			// Find the shortest matching relative path
			headerInfo.IncludePath = Factory.GetModuleShortestIncludePath(headerFile.Module, headerFile.FilePath);

			// Convert the file path to a C identifier
			string filePath = headerFile.FilePath;
			bool isRelative = !Path.IsPathRooted(filePath);
			if (!isRelative && Session.EngineDirectory != null)
			{
				string? directory = Path.GetDirectoryName(Session.EngineDirectory);
				if (!String.IsNullOrEmpty(directory))
				{
					filePath = Path.GetRelativePath(directory, filePath);
					isRelative = !Path.IsPathRooted(filePath);
				}
			}
			if (!isRelative && Session.ProjectDirectory != null)
			{
				string? directory = Path.GetDirectoryName(Session.ProjectDirectory);
				if (!String.IsNullOrEmpty(directory))
				{
					filePath = Path.GetRelativePath(directory, filePath);
					isRelative = !Path.IsPathRooted(filePath);
				}
			}
			filePath = filePath.Replace('\\', '/');
			if (isRelative)
			{
				while (filePath.StartsWith("../", StringComparison.Ordinal))
				{
					filePath = filePath[3..];
				}
			}

			char[] outFilePath = new char[filePath.Length + 4];
			outFilePath[0] = 'F';
			outFilePath[1] = 'I';
			outFilePath[2] = 'D';
			outFilePath[3] = '_';
			for (int index = 0; index < filePath.Length; ++index)
			{
				outFilePath[index + 4] = UhtFCString.IsAlnum(filePath[index]) ? filePath[index] : '_';
			}
			headerInfo.FileId = new string(outFilePath);

			foreach (UhtType headerFileChild in headerFile.Children)
			{
				if (headerFileChild is UhtObject obj)
				{
					UhtPackage? package = obj.Outer as UhtPackage;
					if (package == null)
					{
						throw new UhtIceException("Expected type defined in a header to have a package outer");
					}
					InitObjectInfo(builder, package, ref headerInfo, obj);
				}
			}
		}

		private void InitObjectInfo(StringBuilder builder, UhtPackage package, ref HeaderInfo headerInfo, UhtObject obj)
		{
			UhtModule module = package.Module;
			ref ObjectInfo objectInfo = ref ObjectInfos[obj.ObjectTypeIndex];

			builder.Clear();

			// Construct the names used commonly during export
			bool isNonIntrinsicClass = false;
			builder.Append("Z_Construct_U").Append(obj.EngineClassName).AppendOuterNames(obj);

			string engineClassName = obj.EngineClassName;
			if (obj is UhtClass classObj)
			{
				if (!classObj.ClassFlags.HasAnyFlags(EClassFlags.Intrinsic))
				{
					isNonIntrinsicClass = true;
				}
				if (classObj.ClassExportFlags.HasExactFlags(UhtClassExportFlags.HasReplciatedProperties, UhtClassExportFlags.SelfHasReplicatedProperties))
				{
					headerInfo.NeedsPushModelHeaders = true;
				}
				InitVerseInfo(ref headerInfo, classObj);
				if (classObj.ClassType == UhtClassType.NativeInterface)
				{
					if (classObj.AlternateObject != null)
					{
						ObjectInfos[classObj.AlternateObject.ObjectTypeIndex].NativeInterface = classObj;
					}
				}
			}
			else if (obj is UhtScriptStruct scriptStructObj)
			{
				InitVerseInfo(ref headerInfo, scriptStructObj);

				// Check to see if we are a FastArraySerializer and should try to deduce the FastArraySerializerItemType
				// To fulfill that requirement the struct should be derived from FFastArraySerializer and have a single replicated TArrayProperty
				if (scriptStructObj.IsChildOf(FastArraySerializer))
				{
					// If Super is a valid fastarray we mark this struct as a FastArrayProperty as well
					if (scriptStructObj.Super != null && ObjectInfos[scriptStructObj.Super.ObjectTypeIndex].FastArrayProperty != null)
					{
						objectInfo.FastArrayProperty = ObjectInfos[scriptStructObj.Super.ObjectTypeIndex].FastArrayProperty;
					}

					// A valid fastarray cannot have any additional replicated properties.
					foreach (UhtType child in scriptStructObj.Children)
					{
						if (child is UhtProperty property)
						{
							if (!property.PropertyFlags.HasAnyFlags(EPropertyFlags.RepSkip) && property is UhtArrayProperty)
							{
								if (objectInfo.FastArrayProperty != null)
								{
									objectInfo.FastArrayProperty = null;
									break;
								}
								objectInfo.FastArrayProperty = property;
							}
						}
					}
					if (objectInfo.FastArrayProperty != null)
					{
						headerInfo.NeedsFastArrayHeaders = true;
					}
				}
			}
			else if (obj is UhtFunction)
			{
				// The method for EngineClassName returns type specific where in this case we need just the simple return type
				engineClassName = "Function";
			}
			else if (obj is UhtEnum enumObj)
			{
				if (enumObj.IsVerseField)
				{
					headerInfo.NeedsVerseCodeGen = true;
				}
			}

			if (isNonIntrinsicClass)
			{
				objectInfo.RegisteredSingletonName = builder.ToString();
				builder.Append("_NoRegister");
				objectInfo.UnregisteredSingletonName = builder.ToString();

				objectInfo.UnregisteredExternalDecl = $"\t{module.Api}U{engineClassName}* {objectInfo.UnregisteredSingletonName}();\r\n";
				objectInfo.RegisteredExternalDecl = $"\t{module.Api}U{engineClassName}* {objectInfo.RegisteredSingletonName}();\r\n";
			}
			else
			{
				objectInfo.UnregisteredSingletonName = objectInfo.RegisteredSingletonName = builder.ToString();
				objectInfo.UnregisteredExternalDecl = objectInfo.RegisteredExternalDecl = $"\t{module.Api}U{engineClassName}* {objectInfo.RegisteredSingletonName}();\r\n";
			}

			// Init the children
			foreach (UhtType child in obj.Children)
			{
				if (child is UhtObject childObject)
				{
					InitObjectInfo(builder, package, ref headerInfo, childObject);
				}
			}
		}

		/// <summary>
		/// Given a structure (class or script struct), initialize the header info based on the existence of verse elements
		/// </summary>
		/// <param name="headerInfo">Header information to be initialized</param>
		/// <param name="structObj">Structure in question</param>
		static void InitVerseInfo(ref HeaderInfo headerInfo, UhtStruct structObj)
		{
			if (structObj.IsVerseField)
			{
				headerInfo.NeedsVerseCodeGen = true;
			}
			foreach (UhtType childType in structObj.Children)
			{
				if (childType is UhtProperty childProperty)
				{
					if (childProperty.PropertyExportFlags.HasAnyFlags(UhtPropertyExportFlags.TVal))
					{
						headerInfo.NeedsVerseField = true;
					}
					if (childProperty.PropertyExportFlags.HasAnyFlags(UhtPropertyExportFlags.TPtr))
					{
						headerInfo.NeedsVersePointer = true;
					}
					if (childProperty.PropertyExportFlags.HasAnyFlags(UhtPropertyExportFlags.TVerseTask))
					{
						headerInfo.NeedsVerseTask = true;
					}
					if (!headerInfo.NeedsVerseValue && childProperty is UhtVerseValueProperty)
					{
						headerInfo.NeedsVerseValue = true;
					}
					if (!headerInfo.NeedsVerseString && childProperty is UhtVerseStringProperty)
					{
						headerInfo.NeedsVerseString = true;
					}
				}
			}
		}
		#endregion

		#region Utility functions
		/// <summary>
		/// Return a module's sorted header file list of all header files that or referenced or have declarations.
		/// </summary>
		/// <param name="module">The module in question</param>
		/// <returns>Sorted list of the header files</returns>
		private static List<UhtHeaderFile> GetSortedHeaderFiles(UhtModule module)
		{
			List<UhtHeaderFile> sortedHeaders = new(module.Headers.Count);
			foreach (UhtHeaderFile headerFile in module.Headers)
			{
				if (headerFile.ShouldExport)
				{
					sortedHeaders.Add(headerFile);
				}
			}
			sortedHeaders.Sort((lhs, rhs) => { return StringComparerUE.OrdinalIgnoreCase.Compare(lhs.FilePath, rhs.FilePath); });
			return sortedHeaders;
		}
		#endregion
	}
}
