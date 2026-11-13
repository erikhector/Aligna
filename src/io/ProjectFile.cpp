#include "io/ProjectFile.h"
#include <cmath>

namespace aligna
{
namespace
{
constexpr int kFormat = 1;

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
        return static_cast<SamplePos>(s.getLargeIntValue());
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
            clips.add(juce::var(co));
        }
        o->setProperty("clips", clips);
        tracks.add(juce::var(o));
    }
    root->setProperty("tracks", tracks);

    auto text = juce::JSON::toString(rootVar);

    if (!projectDir.isDirectory())
    {
        error = "Folder does not exist: " + projectDir.getFullPathName();
        return false;
    }
    juce::TemporaryFile temp(file);
    if (!temp.getFile().replaceWithText(text, false, false, "\n"))
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
    if (!file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName();
        return std::nullopt;
    }
    juce::var root;
    auto res = juce::JSON::parse(file.loadFileAsString(), root);
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
        error = "This project was saved by a newer version of Aligna (format " + juce::String(static_cast<int>(format)) + ")";
        return std::nullopt;
    }
    if (format != kFormat)
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

    if (!(p.sampleRate > 0)) { error = "Invalid sampleRate: must be greater than 0"; return std::nullopt; }
    if (!(p.bpm >= 20 && p.bpm <= 400)) { error = "Invalid bpm: must be between 20 and 400"; return std::nullopt; }
    if (!(bpb >= 1 && bpb <= 32 && bpb == std::floor(bpb))) { error = "Invalid beatsPerBar: must be a whole number from 1 to 32"; return std::nullopt; }
    if (!(unit == 1 || unit == 2 || unit == 4 || unit == 8 || unit == 16)) { error = "Invalid beatUnit: must be 1, 2, 4, 8 or 16"; return std::nullopt; }
    p.beatsPerBar = static_cast<int>(bpb);
    p.beatUnit = static_cast<int>(unit);

    auto projectDir = file.getParentDirectory();

    auto* sources = r.array(root, "sources", {});
    if (r.failed()) return fail();
    for (auto& sv : *sources)
    {
        SourceInfo s;
        auto ctx = inFile("sources");
        s.id = r.str(sv, "id", ctx);
        auto path = r.str(sv, "path", ctx);
        s.length = r.pos(sv, "length", ctx);
        s.numChannels = static_cast<int>(r.num(sv, "numChannels", ctx));
        if (r.failed()) return fail();
        juce::File f = juce::File::isAbsolutePath(path) ? juce::File(path) : projectDir.getChildFile(path);
        s.path = f.getFullPathName();
        s.missing = !f.existsAsFile();
        p.sources.push_back(std::move(s));
    }

    auto* tracks = r.array(root, "tracks", {});
    if (r.failed()) return fail();
    for (auto& tv : *tracks)
    {
        Track t;
        auto ctx = inFile("tracks");
        t.id = r.str(tv, "id", ctx);
        t.name = r.str(tv, "name", ctx);
        t.volumeDb = static_cast<float>(r.num(tv, "volumeDb", ctx));
        t.pan = static_cast<float>(r.num(tv, "pan", ctx));
        t.mute = r.boolean(tv, "mute", ctx);
        t.solo = r.boolean(tv, "solo", ctx);
        auto* clips = r.array(tv, "clips", ctx);
        if (r.failed()) return fail();
        for (auto& cv : *clips)
        {
            Clip c;
            auto cctx = inFile("clips of track \"" + t.id + "\"");
            c.id = r.str(cv, "id", cctx);
            c.sourceId = r.str(cv, "sourceId", cctx);
            c.start = r.pos(cv, "start", cctx);
            c.sourceOffset = r.pos(cv, "sourceOffset", cctx);
            c.length = r.pos(cv, "length", cctx);
            c.gainDb = static_cast<float>(r.num(cv, "gainDb", cctx));
            c.fadeIn = r.pos(cv, "fadeIn", cctx);
            c.fadeOut = r.pos(cv, "fadeOut", cctx);
            if (r.failed()) return fail();
            if (findSource(p, c.sourceId) == nullptr)
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
