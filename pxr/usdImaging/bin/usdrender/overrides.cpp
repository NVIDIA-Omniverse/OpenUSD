#include "overrides.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Hold one override after its target schema and USDA value have been resolved.
// Keeping source text alongside the normalized fields makes every later
// conflict or authoring failure attributable to one command-line argument.
struct _ParsedOverride
{
    std::string source;
    SdfPath primPath;
    TfToken attributeName;
    SdfValueTypeName typeName;
    SdfVariability variability = SdfVariabilityVarying;
    bool custom = false;
    VtValue value;
};

std::string
_Trim(const std::string& text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string
_Lower(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

bool
_Fail(const std::string& source, const std::string& message, std::string* error)
{
    *error = "Invalid --set '" + source + "': " + message;
    return false;
}

bool
_HasOnlyInfoKeys(std::vector<TfToken> infoKeys,
                 std::vector<TfToken> allowedKeys)
{
    // Treat parser-generated default fields as optional while rejecting any
    // metadata smuggled through value text. Sdf does not guarantee that it
    // will continue authoring default-valued fields into the scratch layer.
    std::sort(infoKeys.begin(), infoKeys.end());
    std::sort(allowedKeys.begin(), allowedKeys.end());
    return std::includes(allowedKeys.begin(), allowedKeys.end(),
                         infoKeys.begin(), infoKeys.end());
}

bool
_ExpandSettings(const std::string& source, const SdfPath& settingsPath,
                std::string* target, std::string* error)
{
    // Expansion is restricted to a whole leading path component so the token
    // cannot alter an attribute name or concatenate an unintended prim name.
    static const std::string token = "{settings}";
    const std::string lower = _Lower(*target);
    const size_t tokenPosition = lower.find(token);
    if (tokenPosition == std::string::npos) {
        return true;
    }
    if (tokenPosition != 0 ||
        (target->size() > token.size() && (*target)[token.size()] != '/' &&
         (*target)[token.size()] != '.')) {
        return _Fail(source, "{settings} must be a whole path prefix", error);
    }
    if (lower.find(token, token.size()) != std::string::npos) {
        return _Fail(source, "{settings} may appear only once", error);
    }
    if (settingsPath.IsEmpty()) {
        return _Fail(source,
                     "used {settings} but no RenderSettings prim was specified "
                     "or authored in stage metadata",
                     error);
    }
    target->replace(0, token.size(), settingsPath.GetString());
    return true;
}

bool
_ParseHeader(const std::string& source, const SdfPath& settingsPath,
             std::string* valueText, _ParsedOverride* parsed,
             bool* hasExplicitType, bool* hasExplicitVariability,
             std::string* explicitType, std::string* error)
{
    // Split only at the first equals sign because USDA string and dictionary
    // values may legitimately contain additional equals signs.
    const size_t equals = source.find('=');
    if (equals == std::string::npos) {
        return _Fail(source, "expected '='", error);
    }
    const std::string header = _Trim(source.substr(0, equals));
    *valueText = _Trim(source.substr(equals + 1));
    if (header.empty() || valueText->empty()) {
        return _Fail(source, "target and value must not be empty", error);
    }

    // The target is always the final word. At most two leading words can name
    // variability and type, which keeps malformed declarations unambiguous.
    std::istringstream stream(header);
    std::vector<std::string> words;
    for (std::string word; stream >> word;) {
        words.push_back(word);
    }
    if (words.empty() || words.size() > 3) {
        return _Fail(source,
                     "expected [uniform|varying] [type] /Prim.attribute",
                     error);
    }

    std::string target = words.back();
    words.pop_back();
    if (!words.empty() &&
        (words.front() == "uniform" || words.front() == "varying")) {
        *hasExplicitVariability = true;
        parsed->variability = words.front() == "uniform"
                                  ? SdfVariabilityUniform
                                  : SdfVariabilityVarying;
        words.erase(words.begin());
    }
    if (!words.empty()) {
        *hasExplicitType = true;
        *explicitType = words.front();
        words.erase(words.begin());
    }
    if (!words.empty()) {
        return _Fail(source,
                     "expected [uniform|varying] [type] /Prim.attribute",
                     error);
    }

    // Normalize the placeholder before validating the path so diagnostics show
    // the command-line source but all stored destinations are absolute paths.
    if (!_ExpandSettings(source, settingsPath, &target, error)) {
        return false;
    }
    // The first period separates prim and property. Periods are not legal in
    // prim paths or attribute identifiers, while namespace colons remain valid.
    const size_t dot = target.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == target.size()) {
        return _Fail(source, "target must be /Prim.attribute", error);
    }
    const std::string primPathText = target.substr(0, dot);
    std::string pathError;
    if (!SdfPath::IsValidPathString(primPathText, &pathError)) {
        return _Fail(source, "invalid prim path '" + primPathText + "': " +
                                 pathError,
                     error);
    }
    parsed->primPath = SdfPath(primPathText);
    const std::string attributeName = target.substr(dot + 1);
    if (!parsed->primPath.IsAbsolutePath() ||
        !parsed->primPath.IsPrimPath()) {
        return _Fail(source, "prim path must be an absolute prim path", error);
    }
    if (!SdfPath::IsValidNamespacedIdentifier(attributeName)) {
        return _Fail(source, "invalid attribute name '" + attributeName + "'",
                     error);
    }
    parsed->attributeName = TfToken(attributeName);
    parsed->source = source;
    return true;
}

bool
_ValidateTarget(const UsdStageRefPtr& stage, bool hasExplicitType,
                bool hasExplicitVariability, const std::string& explicitType,
                _ParsedOverride* parsed, std::string* error)
{
    // Author only onto defined, directly editable prims in the opened stage.
    // This also makes population-mask omissions fail visibly.
    const UsdPrim prim = stage->GetPrimAtPath(parsed->primPath);
    if (!prim || !prim.IsDefined()) {
        return _Fail(parsed->source,
                     "prim <" + parsed->primPath.GetString() +
                         "> does not exist or is not defined",
                     error);
    }
    if (prim.IsInstanceProxy()) {
        return _Fail(parsed->source,
                     "cannot author inside native instance <" +
                         parsed->primPath.GetString() + ">",
                     error);
    }

    // Resolve an explicit type before inspecting the composed attribute so an
    // unknown type never degrades into a new custom attribute declaration.
    SdfValueTypeName declaredType;
    if (hasExplicitType) {
        declaredType = SdfSchema::GetInstance().FindType(explicitType);
        if (!declaredType) {
            return _Fail(parsed->source,
                         "unknown value type '" + explicitType + "'", error);
        }
    }

    // Existing authored or schema-provided attributes define their canonical
    // type, variability, and custom bit. Explicit declarations may confirm but
    // never change those composed characteristics.
    const UsdAttribute attribute = prim.GetAttribute(parsed->attributeName);
    if (attribute) {
        parsed->typeName = attribute.GetTypeName();
        parsed->custom = attribute.IsCustom();
        const SdfVariability composedVariability = attribute.GetVariability();
        if (hasExplicitType && declaredType != parsed->typeName) {
            return _Fail(parsed->source,
                         "declared type '" + explicitType +
                             "' disagrees with composed type '" +
                             parsed->typeName.GetAsToken().GetString() + "'",
                         error);
        }
        if (hasExplicitVariability &&
            parsed->variability != composedVariability) {
            return _Fail(parsed->source,
                         "declared variability disagrees with composed "
                         "variability",
                         error);
        }
        parsed->variability = composedVariability;
        return true;
    }

    // A missing attribute is intentionally the only path that creates custom
    // schema. Requiring its type prevents typoed builtin names from succeeding.
    if (!hasExplicitType) {
        return _Fail(parsed->source,
                     "attribute '" + parsed->attributeName.GetString() +
                         "' is not defined; specify its type",
                     error);
    }
    parsed->typeName = declaredType;
    parsed->custom = true;
    return true;
}

bool
_ParseValue(const std::string& valueText, _ParsedOverride* parsed,
            std::string* error)
{
    // Reuse Sdf's USDA parser so command-line values accept exactly the same
    // syntax and produce exactly the same VtValue types as layer-authored data.
    // The fixed wrapper gives the parser the type information it requires.
    const SdfLayerRefPtr scratch = SdfLayer::CreateAnonymous("set-value.usda");
    const std::string document =
        "#usda 1.0\nover \"_\"\n{\n    " +
        std::string(parsed->variability == SdfVariabilityUniform ? "uniform "
                                                                  : "") +
        parsed->typeName.GetAsToken().GetString() + " _value = " + valueText +
        "\n}\n";
    // Sdf reports parse errors through Tf diagnostics. Capture the first useful
    // parser message and consume the mark so callers receive one scoped error.
    TfErrorMark errorMark;
    const bool imported = scratch->ImportFromString(document);
    if (!imported) {
        std::string parserMessage;
        const TfErrorMark::Iterator firstError = errorMark.GetBegin();
        if (firstError != errorMark.GetEnd()) {
            parserMessage = firstError->GetCommentary();
        }
        errorMark.Clear();
        return _Fail(parsed->source,
                     "value is not valid USDA syntax" +
                         (parserMessage.empty() ? std::string()
                                                : ": " + parserMessage),
                     error);
    }
    errorMark.Clear();

    // Value text is parsed as part of a document, so verify that it produced
    // only the wrapper prim and attribute. These checks prevent closing the
    // wrapper early and injecting prims, relationships, metadata, or samples.
    const SdfPrimSpecHandleVector rootPrims =
        scratch->GetRootPrims().values_as<SdfPrimSpecHandleVector>();
    const std::vector<TfToken> pseudoRootFields =
        scratch->GetPseudoRoot()->ListInfoKeys();
    if (rootPrims.size() != 1 || rootPrims[0]->GetName() != "_" ||
        !rootPrims[0]->GetNameChildren().empty() ||
        rootPrims[0]->GetProperties().size() != 1 ||
        !rootPrims[0]->GetRelationships().empty() ||
        !scratch->GetSubLayerPaths().empty() ||
        !pseudoRootFields.empty() ||
        !_HasOnlyInfoKeys(rootPrims[0]->ListInfoKeys(),
                          {TfToken("specifier")})) {
        return _Fail(parsed->source, "value contains additional layer content",
                     error);
    }
    const SdfAttributeSpecHandle attribute =
        TfDynamic_cast<SdfAttributeSpecHandle>(
            rootPrims[0]->GetProperties().front());
    const std::vector<TfToken> attributeFields =
        attribute ? attribute->ListInfoKeys() : std::vector<TfToken>();
    if (!attribute || attribute->GetName() != "_value" ||
        attribute->GetNumTimeSamples() != 0 ||
        attribute->HasConnectionPaths() ||
        !_HasOnlyInfoKeys(attributeFields,
                          {TfToken("custom"), TfToken("default"),
                           TfToken("typeName"), TfToken("variability")})) {
        return _Fail(parsed->source, "value contains additional layer content",
                     error);
    }
    // Copy only the typed default value out of the isolated scratch layer. No
    // parsed Sdf specs are transferred into the real session layer.
    parsed->value = attribute->GetDefaultValue();
    if (parsed->value.IsEmpty()) {
        return _Fail(parsed->source, "value did not produce a default value",
                     error);
    }
    return true;
}

} // namespace

bool
ApplyAttributeOverrides(const std::vector<std::string>& specs,
                        const UsdStageRefPtr& stage,
                        const SdfLayerRefPtr& session,
                        const SdfPath& settingsPath, std::string* error)
{
    std::vector<_ParsedOverride> parsedOverrides;
    parsedOverrides.reserve(specs.size());

    // Validate every destination and parse every value before the session
    // layer changes, so one bad argument cannot leave partial overrides.
    for (const std::string& source : specs) {
        _ParsedOverride parsed;
        std::string valueText;
        std::string explicitType;
        bool hasExplicitType = false;
        bool hasExplicitVariability = false;
        if (!_ParseHeader(source, settingsPath, &valueText, &parsed,
                          &hasExplicitType, &hasExplicitVariability,
                          &explicitType, error) ||
            !_ValidateTarget(stage, hasExplicitType, hasExplicitVariability,
                            explicitType, &parsed, error) ||
            !_ParseValue(valueText, &parsed, error)) {
            return false;
        }
        parsedOverrides.push_back(parsed);
    }

    // Compatible duplicate destinations retain the final value and create one
    // attribute spec. Conflicts are rejected before any destination is authored.
    std::map<SdfPath, size_t> destinationIndices;
    std::vector<_ParsedOverride> uniqueOverrides;
    for (const _ParsedOverride& parsed : parsedOverrides) {
        const SdfPath destination =
            parsed.primPath.AppendProperty(parsed.attributeName);
        const auto found = destinationIndices.find(destination);
        if (found == destinationIndices.end()) {
            destinationIndices[destination] = uniqueOverrides.size();
            uniqueOverrides.push_back(parsed);
            continue;
        }
        _ParsedOverride& prior = uniqueOverrides[found->second];
        if (prior.typeName != parsed.typeName ||
            prior.variability != parsed.variability ||
            prior.custom != parsed.custom) {
            *error = "Conflicting --set declarations '" + prior.source +
                     "' and '" + parsed.source + "' for <" +
                     destination.GetString() + ">";
            return false;
        }
        prior.value = parsed.value;
        prior.source = parsed.source;
    }

    // Author only after parsing, target validation, and duplicate resolution
    // have all succeeded. The CLI supplies a fresh anonymous wrapper layer, so
    // these creates cannot collide with user-authored session specs.
    for (const _ParsedOverride& parsed : uniqueOverrides) {
        const SdfPrimSpecHandle primSpec =
            SdfCreatePrimInLayer(session, parsed.primPath);
        if (!primSpec) {
            return _Fail(parsed.source, "could not create destination prim spec",
                         error);
        }
        const SdfAttributeSpecHandle attribute = SdfAttributeSpec::New(
            primSpec, parsed.attributeName.GetString(), parsed.typeName,
            parsed.variability, parsed.custom);
        if (!attribute || !attribute->SetDefaultValue(parsed.value)) {
            return _Fail(parsed.source, "could not author destination value",
                         error);
        }
    }
    return true;
}
