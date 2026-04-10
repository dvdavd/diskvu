// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "colorutils.h"

#include <QCoreApplication>
#include <QDir>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

bool extIsOneOf(QStringView ext, std::initializer_list<QStringView> values)
{
    for (const QStringView value : values) {
        if (ext == value) {
            return true;
        }
    }
    return false;
}

QStringView fileExtensionView(const QString& name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= name.size()) {
        return {};
    }
    return QStringView{name}.mid(dot + 1);
}

QColor unknownFileTypeFallbackColor(const TreemapSettings& settings)
{
    QColor color = settings.unknownFileTypeColor;
    color.setAlphaF(settings.unknownFileTypeOpacity);
    return color;
}

// Pack an extension-only string (without leading dot) the same way as packFileExt.
static uint64_t packExtStr(const QString& ext)
{
    const QChar* d = ext.constData();
    const int len = ext.size();
    if (len <= 0 || len > 8) return 0;

    uint64_t result = 0;
    for (int i = 0; i < len; ++i) {
        const char16_t c = d[i].unicode();
        if (c > 127) return 0;
        const uint8_t lc = (c >= u'A' && c <= u'Z') ? c + 32 : static_cast<uint8_t>(c);
        result |= static_cast<uint64_t>(lc) << (i * 8);
    }
    return result;
}

float radicalInverseBase2(unsigned int value)
{
    float result = 0.0f;
    float factor = 0.5f;

    while (value > 0u) {
        result += factor * static_cast<float>(value & 1u);
        value >>= 1u;
        factor *= 0.5f;
    }

    return result;
}

float rootFolderHue(const FileNode* root)
{
    if (!root) {
        return 0.58f;
    }

    return static_cast<float>(qHash(root->computePath()) % 360u) / 360.0f;
}

float configuredFolderHue(const TreemapSettings& settings)
{
    return static_cast<float>(settings.folderBaseColor.hslHueF());
}

QColor presetColorSample(float t, const TreemapSettings& settings)
{
    // depthGradientPresets() constructs i18n strings + QColors on every call.
    // Cache just the stops for the active preset index per thread (scanner uses
    // multiple worker threads), invalidating only when the preset index changes.
    struct StopsCache {
        int presetIdx = -1;
        QList<QColor> stops;
    };
    static thread_local StopsCache cache;

    if (cache.presetIdx != settings.depthGradientPreset) {
        const QList<ColorUtils::DepthGradientPreset> presets = ColorUtils::depthGradientPresets();
        const int idx = std::clamp(settings.depthGradientPreset, 0,
                                   static_cast<int>(presets.size()) - 1);
        cache.presetIdx = settings.depthGradientPreset;
        cache.stops = presets.at(idx).stops;
    }

    t = std::clamp(t, 0.0f, 1.0f);
    if (settings.depthGradientFlipped) {
        t = 1.0f - t;
    }
    return ColorUtils::sampleGradient(cache.stops, t);
}

float fileHueForExtension(QStringView ext)
{
    if (ext.isEmpty()) {
        return 0.0f;
    }

    if (extIsOneOf(ext, {u"cpp", u"c", u"cc", u"cxx", u"h", u"hpp", u"hh", u"rs", u"go",
                         u"py", u"js", u"ts", u"tsx", u"java", u"kt", u"swift"})) {
        return 0.58f;
    }
    if (extIsOneOf(ext, {u"png", u"jpg", u"jpeg", u"gif", u"webp", u"svg", u"psd", u"kra"})) {
        return 0.80f;
    }
    if (extIsOneOf(ext, {u"mp3", u"flac", u"wav", u"ogg", u"m4a", u"aac"})) {
        return 0.34f;
    }
    if (extIsOneOf(ext, {u"mp4", u"mkv", u"mov", u"avi", u"webm"})) {
        return 0.08f;
    }
    if (extIsOneOf(ext, {u"pdf", u"doc", u"docx", u"txt", u"md", u"odt", u"rtf"})) {
        return 0.03f;
    }
    if (extIsOneOf(ext, {u"zip", u"tar", u"gz", u"xz", u"7z", u"rar", u"bz2"})) {
        return 0.12f;
    }

    return static_cast<float>(qHash(ext) % 360u) / 360.0f;
}

} // namespace

namespace ColorUtils {

// Pack up to 8 lowercase ASCII extension chars from a filename into a uint64_t
// (one byte per char, LSB = first extension char). Returns 0 for filenames with
// no dot, empty extension, extension > 8 chars, or any non-ASCII char.
// Used as a cheap hash key that avoids QString allocation + AES string hashing.
uint64_t packFileExt(const QString& name)
{
    const QChar* d = name.constData();
    const int sz = name.size();

    int dot = sz;
    while (--dot > 0 && d[dot].unicode() != u'.') {}
    if (dot <= 0) return 0;

    const int start = dot + 1;
    const int len = sz - start;
    if (len <= 0 || len > 8) return 0;

    uint64_t result = 0;
    for (int i = 0; i < len; ++i) {
        const char16_t c = d[start + i].unicode();
        if (c > 127) return 0;
        const uint8_t lc = (c >= u'A' && c <= u'Z') ? c + 32 : static_cast<uint8_t>(c);
        result |= static_cast<uint64_t>(lc) << (i * 8);
    }
    return result;
}

QList<DepthGradientPreset> depthGradientPresets()
{
    return {
        {QCoreApplication::translate("ColorUtils", "Rainbow"), {
            QColor(QStringLiteral("#FF0000")),  // red
            QColor(QStringLiteral("#FF8700")),  // princeton orange
            QColor(QStringLiteral("#FFD300")),  // gold
            QColor(QStringLiteral("#DEFF0A")),  // lime yellow
            QColor(QStringLiteral("#A1FF0A")),  // slime lime
            QColor(QStringLiteral("#0AFF99")),  // spring green
            QColor(QStringLiteral("#0AEFFF")),  // electric aqua
            QColor(QStringLiteral("#147DF5")),  // azure blue
            QColor(QStringLiteral("#580AFF")),  // electric indigo
            QColor(QStringLiteral("#BE0AFF")),  // hyper magenta
            QColor(QStringLiteral("#FF0000")),  // red (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Ember"), {
            QColor(QStringLiteral("#FFB300")),  // amber
            QColor(QStringLiteral("#C62828")),  // crimson
            QColor(QStringLiteral("#FFB300")),  // amber (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Ocean"), {
            QColor(QStringLiteral("#00BCD4")),  // cyan
            QColor(QStringLiteral("#1A237E")),  // navy
            QColor(QStringLiteral("#00BCD4")),  // cyan (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Forest"), {
            QColor(QStringLiteral("#2E7D32")),  // deep green
            QColor(QStringLiteral("#8BC34A")),  // moss
            QColor(QStringLiteral("#2E7D32")),  // deep green (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Sunset"), {
            QColor(QStringLiteral("#FFCA28")),  // warm yellow
            QColor(QStringLiteral("#FF7043")),  // orange
            QColor(QStringLiteral("#D81B60")),  // rose
            QColor(QStringLiteral("#FFCA28")),  // warm yellow (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Mint"), {
            QColor(QStringLiteral("#26A69A")),  // teal
            QColor(QStringLiteral("#9CCC65")),  // fresh green
            QColor(QStringLiteral("#DCE775")),  // lime
            QColor(QStringLiteral("#26A69A")),  // teal (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Vice City"), {
            QColor(QStringLiteral("#F96BA8")),  // neon pink sky
            QColor(QStringLiteral("#00B3DD")),  // deep ocean blue
            QColor(QStringLiteral("#F96BA8")),  // neon pink sky (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Autumn"), {
            QColor(QStringLiteral("#558B2F")),  // olive green
            QColor(QStringLiteral("#F9A825")),  // golden yellow
            QColor(QStringLiteral("#E65100")),  // burnt orange
            QColor(QStringLiteral("#B71C1C")),  // deep autumn red
            QColor(QStringLiteral("#558B2F")),  // olive green (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Berry"), {
            QColor(QStringLiteral("#8E24AA")),  // violet
            QColor(QStringLiteral("#D81B60")),  // raspberry
            QColor(QStringLiteral("#FF7043")),  // coral
            QColor(QStringLiteral("#8E24AA")),  // violet (loop)
        }},
        {QCoreApplication::translate("ColorUtils", "Heatmap"), {
            QColor(QStringLiteral("#2979FF")),
            QColor(QStringLiteral("#00BCD4")),
            QColor(QStringLiteral("#4CAF50")),
            QColor(QStringLiteral("#CDDC39")),
            QColor(QStringLiteral("#FF9800")),
            QColor(QStringLiteral("#F44336")),
            QColor(QStringLiteral("#2979FF")),
        }},
        {QCoreApplication::translate("ColorUtils", "Aurora"), {
            QColor(QStringLiteral("#26C6DA")),
            QColor(QStringLiteral("#66BB6A")),
            QColor(QStringLiteral("#42A5F5")),
            QColor(QStringLiteral("#7E57C2")),
            QColor(QStringLiteral("#26C6DA")),
        }},
        {QCoreApplication::translate("ColorUtils", "Dusk"), {
            QColor(QStringLiteral("#FF7043")),
            QColor(QStringLiteral("#EC407A")),
            QColor(QStringLiteral("#AB47BC")),
            QColor(QStringLiteral("#3949AB")),
            QColor(QStringLiteral("#1A237E")),
            QColor(QStringLiteral("#FF7043")),
        }},
    };
}

QColor sampleGradient(const QList<QColor>& stops, float t)
{
    if (stops.isEmpty()) {
        return QColor();
    }
    if (stops.size() == 1) {
        return stops.constFirst();
    }
    t = std::clamp(t, 0.0f, 1.0f);
    const float scaled = t * static_cast<float>(stops.size() - 1);
    const int lo = static_cast<int>(scaled);
    if (lo >= stops.size() - 1) {
        return stops.constLast();
    }
    const float frac = scaled - static_cast<float>(lo);
    const QColor& c0 = stops.at(lo);
    const QColor& c1 = stops.at(lo + 1);
    const float r = static_cast<float>(c0.redF()) + (static_cast<float>(c1.redF()) - static_cast<float>(c0.redF())) * frac;
    const float g = static_cast<float>(c0.greenF()) + (static_cast<float>(c1.greenF()) - static_cast<float>(c0.greenF())) * frac;
    const float b = static_cast<float>(c0.blueF()) + (static_cast<float>(c1.blueF()) - static_cast<float>(c0.blueF())) * frac;
    const float a = static_cast<float>(c0.alphaF()) + (static_cast<float>(c1.alphaF()) - static_cast<float>(c0.alphaF())) * frac;
    return QColor::fromRgbF(std::clamp(r, 0.0f, 1.0f),
                            std::clamp(g, 0.0f, 1.0f),
                            std::clamp(b, 0.0f, 1.0f),
                            std::clamp(a, 0.0f, 1.0f));
}

static QColor folderColorForDepthGradient(int depth, const TreemapSettings& settings)
{
    float t = std::fmod(static_cast<float>(std::max(depth - 1, 0)), 10.0f) / 10.0f;
    QColor color = presetColorSample(t, settings);
    float h = 0.0f, s = 0.0f, l = 0.0f, a = 1.0f;
    color.getHslF(&h, &s, &l, &a);
    // sat/brightness controls are centered at 0.5 (= no change); range is ±0.5 additive shift.
    // Additive keeps all gradient stops shifting by the same absolute amount so no stop clips
    // to white/grey before others (multiplicative would amplify existing lightness differences).
    s = std::clamp(s + static_cast<float>(settings.folderColorSaturation) - 0.5f, 0.0f, 1.0f);
    l = std::clamp(l + static_cast<float>(settings.folderColorBrightness) - 0.5f, 0.0f, 1.0f);
    return QColor::fromHslF(std::max(h, 0.0f), s, l, 1.0f);
}

QColor folderColor(int depth, float branchHue, const TreemapSettings& settings)
{
    if (settings.folderColorMode == TreemapSettings::DepthGradient) {
        return folderColorForDepthGradient(depth, settings);
    }
    return folderColorForBranch(depth, branchHue, settings);
}

float normalizedHue(float hue)
{
    hue = std::fmod(hue, 1.0f);
    if (hue < 0.0f) {
        hue += 1.0f;
    }
    return hue;
}

float hueFromColor(const QColor& color, float fallbackHue)
{
    float hue = 0.0f;
    float saturation = 0.0f;
    float lightness = 0.0f;
    float alpha = 1.0f;
    color.getHslF(&hue, &saturation, &lightness, &alpha);
    if (hue < 0.0f || saturation <= 0.01f) {
        return fallbackHue;
    }
    return normalizedHue(hue);
}

float topLevelFolderBranchHue(const QString& name, const TreemapSettings& settings)
{
    if (settings.folderColorMode == TreemapSettings::SingleHue) {
        return normalizedHue(configuredFolderHue(settings));
    }

    // Sample the gradient at a position derived from the folder name so the hue
    // is stable regardless of child ordering (BFS scan order vs size-sorted order).
    // This ensures the scanner's eager assignment and the post-sort assignColors()
    // call produce identical hues, eliminating a visible color jump after each scan.
    const float t = static_cast<float>(qHash(name) % 1000u) / 1000.0f;
    return hueFromColor(presetColorSample(t, settings), 0.58f);
}

float initialFolderBranchHue(const FileNode* root, const TreemapSettings& settings)
{
    if (settings.folderColorMode == TreemapSettings::SingleHue) {
        return normalizedHue(configuredFolderHue(settings));
    }

    return normalizedHue(rootFolderHue(root));
}

// Colour for a folder that has an explicit colour mark.
// Differs from folderColorForBranch:
//   - base lightness is shifted by kMidDepthOffset normal steps so the mark
//     starts at a mid-level shade rather than the extremes of the user's range
//   - per-level step is halved so the mark is still depth-aware but subtler
//   - saturation gets a 30% boost for stronger visual pop
//   - always uses branchHue (ignores depth-gradient preset)
static QColor folderColorForMark(int depth, float branchHue, const TreemapSettings& settings)
{
    const int relativeDepth = std::max(depth - 1, 0);
    const bool singleHue = settings.folderColorMode == TreemapSettings::SingleHue;

    const float rawSat = singleHue
        ? static_cast<float>(settings.folderBaseColor.hslSaturationF())
        : static_cast<float>(settings.folderColorSaturation);
    const float saturation = std::clamp(rawSat * 1.3f, 0.0f, 1.0f);

    const float baseLightness = singleHue
        ? static_cast<float>(settings.folderBaseColor.lightnessF())
        : static_cast<float>(settings.folderColorBrightness);

    // Shift the base shade by 3 normal steps toward the mid-range.
    constexpr int kMidDepthOffset = 3;
    const float stepSize = static_cast<float>(settings.folderColorDarkenPerLevel);
    const float midOffset = stepSize * kMidDepthOffset;
    const float midBase = settings.folderDepthBrightnessMode == TreemapSettings::LightenPerLevel
        ? baseLightness + midOffset
        : baseLightness - midOffset;

    // Half step size for depth variation within the marked branch.
    const float depthStep = (stepSize * 0.5f) * static_cast<float>(relativeDepth);
    const float signedDepthStep = settings.folderDepthBrightnessMode == TreemapSettings::LightenPerLevel
        ? depthStep : -depthStep;

    return QColor::fromHslF(normalizedHue(branchHue), saturation,
                            std::clamp(midBase + signedDepthStep, 0.0f, 1.0f), 1.0f);
}

QColor folderColorForBranch(int depth, float branchHue, const TreemapSettings& settings)
{
    const int relativeDepth = std::max(depth - 1, 0);
    const float hue = normalizedHue(branchHue);
    const bool singleHue = settings.folderColorMode == TreemapSettings::SingleHue;
    const float saturation = singleHue
        ? std::clamp(static_cast<float>(settings.folderBaseColor.hslSaturationF()), 0.0f, 1.0f)
        : std::clamp(static_cast<float>(settings.folderColorSaturation), 0.0f, 1.0f);
    const float baseLightness = singleHue
        ? static_cast<float>(settings.folderBaseColor.lightnessF())
        : static_cast<float>(settings.folderColorBrightness);
    const float depthStep = static_cast<float>(settings.folderColorDarkenPerLevel)
        * static_cast<float>(relativeDepth);
    const float signedDepthStep = settings.folderDepthBrightnessMode == TreemapSettings::LightenPerLevel
        ? depthStep
        : -depthStep;
    const float lightness = std::clamp(baseLightness + signedDepthStep, 0.0f, 1.0f);
    return QColor::fromHslF(hue, saturation, lightness, 1.0f);
}

QColor fileColorForName(const QString& name, const TreemapSettings& settings)
{
    const QStringView ext = fileExtensionView(name);
    if (!ext.isEmpty()) {
        for (const FileTypeGroup& group : settings.fileTypeGroups) {
            for (const QString& e : group.extensions) {
                if (ext.compare(e, Qt::CaseInsensitive) == 0) {
                    return QColor::fromHslF(
                        static_cast<float>(hueFromColor(group.color, fileHueForExtension(ext))),
                        static_cast<float>(settings.fileColorSaturation),
                        static_cast<float>(settings.fileColorBrightness));
                }
            }
        }
    }
    if (!settings.randomColorForUnknownFiles) {
        return unknownFileTypeFallbackColor(settings);
    }
    return QColor::fromHslF(fileHueForExtension(ext),
                            static_cast<float>(settings.fileColorSaturation),
                            static_cast<float>(settings.fileColorBrightness));
}

// Build a packed-extension → QRgb map for O(1) per-file colour lookup.
// Keys are uint64_t produced by packExtStr (same encoding as packFileExt),
// which avoids QString allocation and AES string hashing on the hot path.
static QHash<uint64_t, QRgb> buildFileColorLookup(const TreemapSettings& settings)
{
    QHash<uint64_t, QRgb> lookup;
    for (const FileTypeGroup& group : settings.fileTypeGroups) {
        for (const QString& e : group.extensions) {
            const uint64_t key = packExtStr(e);
            if (key && !lookup.contains(key)) {  // first group wins, matching fileColorForName
                const QString eLower = e.toLower();
                lookup.insert(key, QColor::fromHslF(
                    static_cast<float>(hueFromColor(group.color,
                                                    fileHueForExtension(QStringView(eLower)))),
                    static_cast<float>(settings.fileColorSaturation),
                    static_cast<float>(settings.fileColorBrightness)).rgba());
            }
        }
    }
    return lookup;
}

static QColor fileColorFast(const QString& name,
                            const QHash<uint64_t, QRgb>& lookup,
                            const TreemapSettings& settings)
{
    const uint64_t key = packFileExt(name);
    if (key) {
        const auto it = lookup.find(key);
        if (it != lookup.end()) {
            return QColor::fromRgba(it.value());
        }
    }
    if (!settings.randomColorForUnknownFiles) {
        return unknownFileTypeFallbackColor(settings);
    }
    return QColor::fromHslF(fileHueForExtension(fileExtensionView(name)),
                            static_cast<float>(settings.fileColorSaturation),
                            static_cast<float>(settings.fileColorBrightness));
}

static void assignColorsRecurse(FileNode* node, int depth, float branchHue,
                                const QHash<uint64_t, QRgb>& fileColorLookup,
                                const TreemapSettings& settings,
                                const QString& parentPath,
                                bool inMarkedBranch)
{
    if (!node) {
        return;
    }

    // Prefer absolutePath when set (scan root, and incremental-refresh subtree root
    // which has depth > 0 but no parent in the fresh ScanResult).
    const QString currentPath = !node->absolutePath.isEmpty()
        ? node->absolutePath
        : (parentPath.isEmpty() ? node->name : QDir(parentPath).filePath(node->name));

    if (node->isDirectory) {
        float effectiveHue = branchHue;
        const auto it = settings.folderColorMarks.find(currentPath);
        bool hueChanged = false;
        if (it != settings.folderColorMarks.end()) {
            switch (it.value()) {
                case FolderMark::ColorRed:    effectiveHue = 0.0f; hueChanged = true; break;
                case FolderMark::ColorOrange: effectiveHue = 30.0f / 360.0f; hueChanged = true; break;
                case FolderMark::ColorYellow: effectiveHue = 60.0f / 360.0f; hueChanged = true; break;
                case FolderMark::ColorGreen:  effectiveHue = 120.0f / 360.0f; hueChanged = true; break;
                case FolderMark::ColorBlue:   effectiveHue = 240.0f / 360.0f; hueChanged = true; break;
                case FolderMark::ColorPurple: effectiveHue = 280.0f / 360.0f; hueChanged = true; break;
                default: break;
            }
        }
        if (hueChanged) {
            node->color = folderColorForMark(depth, effectiveHue, settings).rgba();
            branchHue = effectiveHue;
            inMarkedBranch = true;
        } else if (inMarkedBranch) {
            // Inside a marked branch: use the same formula as the mark root so that
            // depth progression is consistent (both start from the same mid-base
            // and use the same half-step size). Using folderColorForBranch here
            // would start from a different base, making children lighter than parent.
            node->color = folderColorForMark(depth, effectiveHue, settings).rgba();
        } else {
            node->color = folderColor(depth, effectiveHue, settings).rgba();
        }
    } else {
        node->color = fileColorFast(node->name, fileColorLookup, settings).rgba();
    }

    for (FileNode* child : node->children) {
        if (!child->isVirtual) {
            assignColorsRecurse(child, depth + 1, branchHue, fileColorLookup, settings, currentPath, inMarkedBranch);
        }
    }
}

void assignColorsForSubtree(FileNode* node, int depth, float branchHue, const TreemapSettings& settings)
{
    const QHash<uint64_t, QRgb> fileColorLookup = buildFileColorLookup(settings);
    assignColorsRecurse(node, depth, branchHue, fileColorLookup, settings, node->parent ? node->parent->computePath() : QString(), false);
}

void assignColorsForSubtree(FileNode* node, int depth, float branchHue, bool inMarkedBranch, const TreemapSettings& settings)
{
    const QHash<uint64_t, QRgb> fileColorLookup = buildFileColorLookup(settings);
    assignColorsRecurse(node, depth, branchHue, fileColorLookup, settings, node->parent ? node->parent->computePath() : QString(), inMarkedBranch);
}

static void assignColorsWithLookup(FileNode* root, const QHash<uint64_t, QRgb>& fileColorLookup,
                                   const TreemapSettings& settings)
{
    if (!root) {
        return;
    }

    float rootHue = initialFolderBranchHue(root, settings);
    const auto it = settings.folderColorMarks.find(root->absolutePath);
    bool rootHueChanged = false;
    if (it != settings.folderColorMarks.end()) {
        switch (it.value()) {
            case FolderMark::ColorRed:    rootHue = 0.0f; rootHueChanged = true; break;
            case FolderMark::ColorOrange: rootHue = 30.0f / 360.0f; rootHueChanged = true; break;
            case FolderMark::ColorYellow: rootHue = 60.0f / 360.0f; rootHueChanged = true; break;
            case FolderMark::ColorGreen:  rootHue = 120.0f / 360.0f; rootHueChanged = true; break;
            case FolderMark::ColorBlue:   rootHue = 240.0f / 360.0f; rootHueChanged = true; break;
            case FolderMark::ColorPurple: rootHue = 280.0f / 360.0f; rootHueChanged = true; break;
            default: break; // Category marks don't change the color
        }
    }

    root->color = rootHueChanged
        ? folderColorForMark(0, rootHue, settings).rgba()
        : folderColor(0, rootHue, settings).rgba();

    for (FileNode* child : root->children) {
        if (!child->isVirtual) {
            assignColorsRecurse(child, 1, topLevelFolderBranchHue(child->name, settings),
                                fileColorLookup, settings, root->absolutePath, rootHueChanged);
        }
    }
}

void assignColors(FileNode* root, const TreemapSettings& settings)
{
    assignColorsWithLookup(root, buildFileColorLookup(settings), settings);
}

void assignColors(FileNode* root, FileNode* liveRoot, const TreemapSettings& settings)
{
    const QHash<uint64_t, QRgb> fileColorLookup = buildFileColorLookup(settings);
    assignColorsWithLookup(root, fileColorLookup, settings);
    assignColorsWithLookup(liveRoot, fileColorLookup, settings);
}

QString fileTypeLabelForName(const QString& name)
{
    const QStringView ext = fileExtensionView(name);
    if (ext.isEmpty()) {
        return QCoreApplication::translate("ColorUtils", "No extension");
    }

    return QStringLiteral(".") + ext.toString().toLower();
}

QString fileTypeLabelForNode(const FileNode* node)
{
    if (!node) {
        return {};
    }
    if (node->isVirtual) {
        return QCoreApplication::translate("ColorUtils", "Free Space");
    }
    if (node->isDirectory) {
        return {};
    }
    return fileTypeLabelForName(node->name);
}

} // namespace ColorUtils
