#include "io/ProjectFile.h"
#include <algorithm>
#include <cmath>
#include <set>
#if !JUCE_WINDOWS
#include <sys/stat.h>
#endif

namespace aligna
{
namespace
{
constexpr int kFormat = 2;

// Limits for files we are willing to load. A project file may come from anyone.
constexpr juce::int64 kMaxFileBytes = 64 * 1024 * 1024;
constexpr int kMaxJsonDepth = 64; // a valid project nests 4 deep
// 24 hours at 384 kHz. Sums of a few positions stay far below the int64 range.
constexpr SamplePos kMaxSamplePos = 24LL * 3600 * 384000;
constexpr int kMaxSources = 10'000, kMaxTracks = 10'000, kMaxClips = 1'000'000;
// Same ranges as the track header sliders (TrackHeader.cpp) and setClipGain (ClipOps.cpp).
constexpr double kMinGainDb = -60.0, kMaxGainDb = 12.0;
constexpr double kMinPan = -1.0, kMaxPan = 1.0;

float gainFromFile(double db) { return static_cast<float>(std::clamp(db, kMinGainDb, kMaxGainDb)); }
float panFromFile(double pan) { return static_cast<float>(std::clamp(pan, kMinPan, kMaxPan)); }

// True for UNC paths (two leading slashes of either kind, as in //host/share).
// Looking at such a path can start a network connection.
bool isNetworkPath(const juce::String& path)
{
    auto isSlash = [](juce::juce_wchar c) { return c == '/' || c == '\\'; };
    return path.length() >= 2 && isSlash(path[0]) && isSlash(path[1]);
}

// A regular file, or a link to one. A FIFO or a device is not: opening it could block forever.
bool isRegularFile(const juce::File& f)
{
#if JUCE_WINDOWS
    return f.existsAsFile();
#else
    struct stat st;
    return stat(f.getFullPathName().toRawUTF8(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

// Deepest nesting of [ ] and { } outside strings. The JSON parser recurses once per level,
// so this is checked before parsing. Stops counting once the depth passes maxDepth.
int jsonNestingDepth(const juce::String& text, int maxDepth)
{
    int depth = 0, deepest = 0;
    juce::juce_wchar quote = 0; // the open quote character while inside a string
    for (auto t = text.getCharPointer(); !t.isEmpty();)
    {
        const auto c = t.getAndAdvance();
        if (quote != 0)
        {
            if (c == '\\')
            {
                if (!t.isEmpty())
                    ++t;
            }
            else if (c == quote)
                quote = 0;
        }
        else if (c == '"' || c == '\'') // the parser takes both kinds of quote
            quote = c;
        else if (c == '[' || c == '{')
        {
            deepest = std::max(deepest, ++depth);
            if (deepest > maxDepth)
                break;
        }
        else if (c == ']' || c == '}')
            depth = std::max(0, depth - 1);
    }
    return deepest;
}

juce::var posToVar(SamplePos v) { return juce::String(static_cast<juce::int64>(v)); }

juce::String storedPath(const juce::File& audio, const juce::File& projectDir)
{
    juce::String p = audio.isAChildOf(projectDir) ? audio.getRelativePathFrom(projectDir)
                                                  : audio.getFullPathName();
    return p.replaceCharacter('\\', '/');
}

struct Reader
{
    juce::String error;
    bool failed() const { return error.isNotEmpty(); }

    const juce::var* field(const juce::var& obj, const char* key, const juce::String& ctx)
    {
        if (failed()) return nullptr;
        auto* o = obj.getDynamicObject();
        if (o == nullptr || !o->hasProperty(key))
        {
            error = "Missing key \"" + juce::String(key) + "\"" + ctx;
            return nullptr;
        }
        return &o->getProperties().getValueAt(o->getProperties().indexOf(key));
    }

    juce::String str(const juce::var& obj, const char* key, const juce::String& ctx)
    {
        auto* v = field(obj, key, ctx);
        if (v == nullptr) return {};
        if (!v->isString())
        {
            error = "Key \"" + juce::String(key) + "\" must be a string" + ctx;
            return {};
        }
        return v->toString();
    }

    double num(const juce::var& obj, const char* key, const juce::String& ctx)
    {
        auto* v = field(obj, key, ctx);
        if (v == nullptr) return 0;
        if (!(v->isInt() || v->isInt64() || v->isDouble()))
        {
            error = "Key \"" + juce::String(key) + "\" must be a number" + ctx;
            return 0;
        }
        auto d = static_cast<double>(*v);
        if (!std::isfinite(d))
        {
            error = "Key \"" + juce::String(key) + "\" must be finite" + ctx;
            return 0;
        }
        return d;
    }

    bool boolean(const juce::var& obj, const char* key, const juce::String& ctx)
    {
        auto* v = field(obj, key, ctx);
        if (v == nullptr) return false;
        if (!v->isBool())
        {
            error = "Key \"" + juce::String(key) + "\" must be true or false" + ctx;
            return false;
        }
        return static_cast<bool>(*v);
    }

    SamplePos pos(const juce::var& obj, const char* key, const juce::String& ctx)
    {
        auto s = str(obj, key, ctx);
        if (failed()) return 0;
        if (s.isEmpty() || !s.containsOnly("0123456789") || s.length() > 18)
        {
            error = "Key \"" + juce::String(key) + "\" is not a valid sample position: \"" + s + "\"" + ctx;
            return 0;
        }
        auto v = static_cast<SamplePos>(s.getLargeIntValue());
        if (v > kMaxSamplePos)
        {
            error = "Key \"" + juce::String(key) + "\" is too large: \"" + s + "\"" + ctx;
            return 0;
        }
        return v;
    }

    const juce::Array<juce::var>* array(const juce::var& obj, const char* key, const juce::String& ctx)
    {
        auto* v = field(obj, key, ctx);
        if (v == nullptr) return nullptr;
        if (!v->isArray())
        {
            error = "Key \"" + juce::String(key) + "\" must be an array" + ctx;
            return nullptr;
        }
        return v->getArray();
    }
};

juce::String inFile(const juce::String& what) { return " (in " + what + ")"; }
} // namespace

bool saveProject(const Project& p, const juce::File& file, juce::String& error)
{
    auto projectDir = file.getParentDirectory();

    auto* root = new juce::DynamicObject();
    juce::var rootVar(root);
    root->setProperty("format", kFormat);
    root->setProperty("sampleRate", p.sampleRate);
    root->setProperty("bpm", p.bpm);
    root->setProperty("beatsPerBar", p.beatsPerBar);
    root->setProperty("beatUnit", p.beatUnit);

    juce::Array<juce::var> sources;
    for (auto& s : p.sources)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", s.id);
        o->setProperty("path", storedPath(juce::File(s.path), projectDir));
        o->setProperty("length", posToVar(s.length));
        o->setProperty("numChannels", s.numChannels);
        sources.add(juce::var(o));
    }
    root->setProperty("sources", sources);

    juce::Array<juce::var> tracks;
    for (auto& t : p.tracks)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", t.id);
        o->setProperty("name", t.name);
        o->setProperty("volumeDb", static_cast<double>(t.volumeDb));
        o->setProperty("pan", static_cast<double>(t.pan));
        o->setProperty("mute", t.mute);
        o->setProperty("solo", t.solo);
        juce::Array<juce::var> clips;
        for (auto& c : t.clips)
        {
            auto* co = new juce::DynamicObject();
            co->setProperty("id", c.id);
            co->setProperty("sourceId", c.sourceId);
            co->setProperty("start", posToVar(c.start));
            co->setProperty("sourceOffset", posToVar(c.sourceOffset));
            co->setProperty("length", posToVar(c.length));
            co->setProperty("gainDb", static_cast<double>(c.gainDb));
            co->setProperty("fadeIn", posToVar(c.fadeIn));
            co->setProperty("fadeOut", posToVar(c.fadeOut));
            co->setProperty("stretch", c.stretch);
            co->setProperty("sourceBpm", c.sourceBpm);
            clips.add(juce::var(co));
        }
        o->setProperty("clips", clips);
        tracks.add(juce::var(o));
    }
    root->setProperty("tracks", tracks);

    // JSON::toString ends lines with CR LF; the file uses LF. Strings hold no raw line breaks.
    auto text = juce::JSON::toString(rootVar).replace("\r\n", "\n");

    if (!projectDir.isDirectory())
    {
        error = "Folder does not exist: " + projectDir.getFullPathName();
        return false;
    }
    juce::TemporaryFile temp(file);
    // Every step is checked: a short write (disk full) must never replace the previous file.
    const auto numBytes = text.getNumBytesAsUTF8();
    bool written = false;
    {
        juce::FileOutputStream out(temp.getFile());
        if (out.openedOk())
        {
            written = out.setPosition(0) && out.truncate().wasOk() && out.write(text.toRawUTF8(), numBytes);
            out.flush();
            written = written && out.getStatus().wasOk();
        }
    }
    if (!written || temp.getFile().getSize() != static_cast<juce::int64>(numBytes))
    {
        error = "Could not write " + temp.getFile().getFullPathName();
        temp.getFile().deleteFile();
        return false;
    }
    if (!temp.overwriteTargetFileWithTemporary())
    {
        error = "Could not replace " + file.getFullPathName();
        return false;
    }
    return true;
}

std::optional<Project> loadProject(const juce::File& file, juce::String& error)
{
    error.clear();
    if (!isRegularFile(file))
    {
        error = "File not found: " + file.getFullPathName();
        return std::nullopt;
    }
    if (file.getSize() > kMaxFileBytes)
    {
        error = "Not a valid project file: larger than " + juce::String(kMaxFileBytes / (1024 * 1024)) + " MB";
        return std::nullopt;
    }
    auto text = file.loadFileAsString();
    if (jsonNestingDepth(text, kMaxJsonDepth) > kMaxJsonDepth)
    {
        error = "Not a valid project file: nested more than " + juce::String(kMaxJsonDepth) + " levels deep";
        return std::nullopt;
    }
    juce::var root;
    auto res = juce::JSON::parse(text, root);
    if (res.failed() || root.getDynamicObject() == nullptr)
    {
        error = "Not a valid project file: " + (res.failed() ? res.getErrorMessage() : juce::String("expected a JSON object"));
        return std::nullopt;
    }

    Reader r;
    auto fail = [&]() -> std::optional<Project> { error = r.error; return std::nullopt; };

    auto format = r.num(root, "format", {});
    if (r.failed()) return fail();
    if (format > kFormat)
    {
        error = "This project was saved by a newer version of Aligna (format " + juce::String(format) + ")";
        return std::nullopt;
    }
    if (format < 1)
    {
        error = "Unsupported project format " + juce::String(format);
        return std::nullopt;
    }

    Project p;
    p.sampleRate = r.num(root, "sampleRate", {});
    p.bpm = r.num(root, "bpm", {});
    auto bpb = r.num(root, "beatsPerBar", {});
    auto unit = r.num(root, "beatUnit", {});
    if (r.failed()) return fail();

    if (!(p.sampleRate >= 8000 && p.sampleRate <= 384000)) { error = "Invalid sampleRate: must be between 8000 and 384000"; return std::nullopt; }
    if (!(p.bpm >= 20 && p.bpm <= 400)) { error = "Invalid bpm: must be between 20 and 400"; return std::nullopt; }
    if (!(bpb >= 1 && bpb <= 32 && !(std::floor(bpb) < bpb))) { error = "Invalid beatsPerBar: must be a whole number from 1 to 32"; return std::nullopt; }
    const int unitInt = (unit >= 1 && unit <= 16 && std::floor(unit) < unit + 0.5 && !(std::floor(unit) < unit)) ? static_cast<int>(unit) : 0;
    if (!(unitInt == 1 || unitInt == 2 || unitInt == 4 || unitInt == 8 || unitInt == 16)) { error = "Invalid beatUnit: must be 1, 2, 4, 8 or 16"; return std::nullopt; }
    p.beatsPerBar = static_cast<int>(bpb);
    p.beatUnit = unitInt;

    auto projectDir = file.getParentDirectory();

    auto* sources = r.array(root, "sources", {});
    if (r.failed()) return fail();
    if (sources->size() > kMaxSources)
    {
        error = "Too many sources: at most " + juce::String(kMaxSources) + " are supported";
        return std::nullopt;
    }
    std::set<juce::String> sourceIds, trackIds, clipIds;
    for (auto& sv : *sources)
    {
        SourceInfo s;
        auto ctx = inFile("sources");
        s.id = r.str(sv, "id", ctx);
        auto path = r.str(sv, "path", ctx);
        s.length = r.pos(sv, "length", ctx);
        auto nch = r.num(sv, "numChannels", ctx);
        if (r.failed()) return fail();
        if (!(nch >= 1 && nch <= 2) || std::floor(nch) < nch)
        {
            error = "Invalid numChannels: must be 1 or 2" + ctx;
            return std::nullopt;
        }
        s.numChannels = static_cast<int>(nch);
        if (!sourceIds.insert(s.id).second)
        {
            error = "Duplicate source id \"" + s.id + "\"";
            return std::nullopt;
        }
        if (isNetworkPath(path))
        {
            // Never looked up: the path stays as written and the source counts as missing.
            s.path = path;
            s.missing = true;
        }
        else
        {
            juce::File f = juce::File::isAbsolutePath(path) ? juce::File(path) : projectDir.getChildFile(path);
            s.path = f.getFullPathName();
            s.missing = !isRegularFile(f);
        }
        p.sources.push_back(std::move(s));
    }

    auto* tracks = r.array(root, "tracks", {});
    if (r.failed()) return fail();
    if (tracks->size() > kMaxTracks)
    {
        error = "Too many tracks: at most " + juce::String(kMaxTracks) + " are supported";
        return std::nullopt;
    }
    for (auto& tv : *tracks)
    {
        Track t;
        auto ctx = inFile("tracks");
        t.id = r.str(tv, "id", ctx);
        t.name = r.str(tv, "name", ctx);
        t.volumeDb = gainFromFile(r.num(tv, "volumeDb", ctx));
        t.pan = panFromFile(r.num(tv, "pan", ctx));
        t.mute = r.boolean(tv, "mute", ctx);
        t.solo = r.boolean(tv, "solo", ctx);
        auto* clips = r.array(tv, "clips", ctx);
        if (r.failed()) return fail();
        if (!trackIds.insert(t.id).second)
        {
            error = "Duplicate track id \"" + t.id + "\"";
            return std::nullopt;
        }
        if (clips->size() > kMaxClips - static_cast<int>(clipIds.size()))
        {
            error = "Too many clips: at most " + juce::String(kMaxClips) + " are supported";
            return std::nullopt;
        }
        for (auto& cv : *clips)
        {
            Clip c;
            auto cctx = inFile("clips of track \"" + t.id + "\"");
            c.id = r.str(cv, "id", cctx);
            c.sourceId = r.str(cv, "sourceId", cctx);
            c.start = r.pos(cv, "start", cctx);
            c.sourceOffset = r.pos(cv, "sourceOffset", cctx);
            c.length = r.pos(cv, "length", cctx);
            c.gainDb = gainFromFile(r.num(cv, "gainDb", cctx));
            c.fadeIn = r.pos(cv, "fadeIn", cctx);
            c.fadeOut = r.pos(cv, "fadeOut", cctx);
            if (r.failed()) return fail();
            if (c.length < 1)
            {
                error = "Invalid length: must be at least 1" + cctx;
                return std::nullopt;
            }
            if (!clipIds.insert(c.id).second)
            {
                error = "Duplicate clip id \"" + c.id + "\"";
                return std::nullopt;
            }
            // Optional keys (format 1 has neither): a missing key means the default.
            if (auto* o = cv.getDynamicObject(); o != nullptr && o->hasProperty("stretch"))
            {
                c.stretch = r.num(cv, "stretch", cctx);
                if (r.failed()) return fail();
                if (!(c.stretch >= kMinStretch && c.stretch <= kMaxStretch))
                {
                    error = "Invalid stretch: must be between 0.25 and 4" + cctx;
                    return std::nullopt;
                }
            }
            if (auto* o = cv.getDynamicObject(); o != nullptr && o->hasProperty("sourceBpm"))
            {
                c.sourceBpm = r.num(cv, "sourceBpm", cctx);
                if (r.failed()) return fail();
                if (!(nearlyEqual(c.sourceBpm, 0) || (c.sourceBpm >= 20 && c.sourceBpm <= 400)))
                {
                    error = "Invalid sourceBpm: must be 0 or between 20 and 400" + cctx;
                    return std::nullopt;
                }
            }
            if (sourceIds.count(c.sourceId) == 0)
            {
                error = "Clip \"" + c.id + "\" refers to unknown source \"" + c.sourceId + "\"";
                return std::nullopt;
            }
            t.clips.push_back(std::move(c));
        }
        p.tracks.push_back(std::move(t));
    }
    return p;
}
} // namespace aligna
