// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include <cstdint>

namespace VerseFN
{
    // A place to put Fortnite upload versions that we use to gate some compiler features
    namespace UploadedAtFNVersion
    {
        // This is expected to follow the Fortnite release convention. For example, `28.20` would be `2820`.

        // Leave a few "versions" that occur *after* Latest. The default version in test situations is always Latest. This will give us some room for testing.
        static constexpr uint32_t Latest = UINT32_MAX - 0xFF;
        static constexpr uint32_t Primordial = 0;
        inline bool CheckSuperQualifiers(const uint32_t CurrentVersion)                    { return CurrentVersion >= 2810; }
        inline bool EnforceDontMixCommaAndSemicolonInBlocks(const uint32_t CurrentVersion) { return CurrentVersion == 2820; }
        inline bool EnableGenerators(const uint32_t CurrentVersion)                        { return CurrentVersion >= 2930; }
        inline bool EnableFinalSpecifierFixes(const uint32_t CurrentVersion)               { return CurrentVersion >= 2930; }
        inline bool EnableNamedParametersForLocalize(const uint32_t CurrentVersion)        { return CurrentVersion >= 2930; }
        inline bool EnableProfileMacro(const uint32_t CurrentVersion)                      { return CurrentVersion >= 2930; }
        inline bool EnforceUnambiguousEnumerators(const uint32_t CurrentVersion)           { return CurrentVersion >= 3000; }
        inline bool StricterCheckForDefaultInConcreteClasses(const uint32_t CurrentVersion){ return CurrentVersion >= 3000; }
        inline bool SortSourceFilesLexicographically(const uint32_t CurrentVersion)        { return CurrentVersion >= 3010; }
        inline bool DeprecateVariesEffect(const uint32_t CurrentVersion)                   { return CurrentVersion >= 3100; }
        inline bool ConcurrencyAddScope(const uint32_t CurrentVersion)                     { return CurrentVersion >= 3100; }
        inline bool DecidesEffectNoLongerImpliesComputes(const uint32_t CurrentVersion)    { return CurrentVersion >= 3100; }
        inline bool StricterEditableOverrideCheck(const uint32_t CurrentVersion)           { return CurrentVersion >= 3200; }
        inline bool OptionTypeDoesntIgnoreValueHashability(const uint32_t CurrentVersion)  { return CurrentVersion >= 3100; }
        inline bool SortSourceSubmodulesLexicographically(const uint32_t CurrentVersion)   { return CurrentVersion >= 3200; }
        inline bool EnforceSnippetNameValidity(const uint32_t CurrentVersion)              { return CurrentVersion >= 3200; }
        inline bool EnableCastableSubtype(const uint32_t CurrentVersion)                   { return CurrentVersion >= 3200; }
        inline bool EnforceCorrectQualifiedNames(const uint32_t CurrentVersion)            { return CurrentVersion >= 3300; }
        inline bool StrictConstructorFunctionInvocation(const uint32_t CurrentVersion)     { return CurrentVersion >= 3300; }
        inline bool EnforceUECaseInsensitiveNames(const uint32_t CurrentVersion)           { return CurrentVersion >= 3300; }
        inline bool AttributesRequireComputes(const uint32_t CurrentVersion)               { return CurrentVersion >= 3300; }
        inline bool DisallowNonClassEditableSubtypes(const uint32_t CurrentVersion)        { return CurrentVersion >= 3400; }
        inline bool DetectInaccessibleTypeDependenciesLate(const uint32_t CurrentVersion)  { return CurrentVersion >= 3400; }
        inline bool DisallowInstanceInAttributeExpression(const uint32_t CurrentVersion)   { return CurrentVersion >= 3410; }
        inline bool EnforceConcreteInterfaceData(const uint32_t CurrentVersion)            { return CurrentVersion >= 3420; }
        inline bool PersistableClassesMustNotImplementInterfaces(const uint32_t CurrentVersion) { return CurrentVersion >= 3430; }
        inline bool RelaxInstancedReferenceSemantics(const uint32_t CurrentVersion)          { return CurrentVersion >= 3600; }
        inline bool DisallowSetExprOutsideAssignment(const uint32_t CurrentVersion)          { return CurrentVersion >= 3500; }
        inline bool EnforceNoReservedWordsAsEnumerators(const uint32_t CurrentVersion)       { return CurrentVersion >= 3430; }
        inline bool AllowEnumeratorsToAliasBuiltinDefinitions(const uint32_t CurrentVersion) { return CurrentVersion <  3430; }
        inline bool DetectInaccessibleTypeArguments(const uint32_t CurrentVersion)           { return CurrentVersion >= 3600; }
        inline bool EnforceTupleElementExprFallibility(const uint32_t CurrentVersion)        { return CurrentVersion >= 3600; }
        inline bool AllowRedirectorReflection(const uint32_t CurrentVersion)                 { return CurrentVersion >= 3600; }
    }
}
