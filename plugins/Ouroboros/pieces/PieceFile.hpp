#pragma once
// Saved pieces on disk: float32 mono WAV files, piece.txt, and slot folders "01".."16" with atomic replace.
// POSIX only; on Windows (desktop build) pieces are unavailable and everything returns an error/empty value.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include "dsp/Piece.hpp"
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ouro { namespace pieces {

enum class Err { None, NoSpace, Io };   // NoSpace: errno ENOSPC or EDQUOT

#ifndef _WIN32
inline Err errFromErrno() { return errno == ENOSPC || errno == EDQUOT ? Err::NoSpace : Err::Io; }
inline bool isDirectory(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
inline bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
inline void makeDirs(const std::string& p) {   // mkdir -p
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777);
}
inline bool writeAll(int fd, const void* b, size_t n) {
    const char* c = static_cast<const char*>(b);
    while (n) {
        ssize_t w = ::write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        c += w; n -= size_t(w);
    }
    return true;
}
inline bool readAll(int fd, void* b, size_t n) {
    char* c = static_cast<char*>(b);
    while (n) {
        ssize_t r = ::read(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        c += r; n -= size_t(r);
    }
    return true;
}
inline void put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
inline void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
inline uint32_t get32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t get16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
#endif

inline std::string piecesDir() {
#ifdef _WIN32
    return "";
#else
    std::string base;
    const char* e = getenv("MOD_USER_FILES_DIR");
    const char* h = getenv("HOME");
    if (e && *e) base = e;
    else if (isDirectory("/data/user-files")) base = "/data/user-files";
    else base = std::string(h ? h : ".") + "/.ouroboros";
    return base + "/Audio Loops/Ouroboros";
#endif
}
inline std::string slotDir(const std::string& root, int slot) {
    char b[8]; snprintf(b, sizeof b, "/%02d", slot);
    return root + b;
}
inline std::string trackName(int part, int track) {
    return std::string(1, char('A' + part)) + char('1' + track);
}

// 44-byte header (RIFF/WAVE, "fmt " 16 bytes: IEEE float, mono, 32 bit, "data"), then the samples.
class WavWriter {
public:
    WavWriter() {}
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter() {
#ifndef _WIN32
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    bool open(const std::string& path, uint32_t rate) {
#ifdef _WIN32
        (void)path; (void)rate; return false;
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd_ < 0) return false;
        frames_ = 0;
        unsigned char h[44] = {};
        memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
        put16(h + 20, 3); put16(h + 22, 1); put32(h + 24, rate); put32(h + 28, rate * 4);
        put16(h + 32, 4); put16(h + 34, 32); memcpy(h + 36, "data", 4);   // sizes patched by close()
        if (!writeAll(fd_, h, sizeof h)) { ::close(fd_); fd_ = -1; return false; }
        return true;
#endif
    }
    Err write(const float* s, uint32_t n) {
#ifdef _WIN32
        (void)s; (void)n; return Err::Io;
#else
        if (fd_ < 0) return Err::Io;
        if (!writeAll(fd_, s, size_t(n) * 4)) return errFromErrno();
        frames_ += n;
        return Err::None;
#endif
    }
    Err close() {
#ifdef _WIN32
        return Err::Io;
#else
        if (fd_ < 0) return Err::Io;
        unsigned char a[4]; Err e = Err::None;
        put32(a, 36 + frames_ * 4);
        if (pwrite(fd_, a, 4, 4) != 4) e = errFromErrno();
        put32(a, frames_ * 4);
        if (e == Err::None && pwrite(fd_, a, 4, 40) != 4) e = errFromErrno();
        if (e == Err::None && fsync(fd_) != 0) e = errFromErrno();
        if (::close(fd_) != 0 && e == Err::None) e = errFromErrno();
        fd_ = -1;
        return e;
#endif
    }
private:
    int fd_ = -1;
    uint32_t frames_ = 0;
};

class WavReader {
public:
    WavReader() {}
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;
    ~WavReader() {
#ifndef _WIN32
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    bool open(const std::string& path) {
#ifdef _WIN32
        (void)path; return false;
#else
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st; unsigned char h[12], c[8], f[16];
        bool fmtOk = false, ok = fstat(fd_, &st) == 0 && readAll(fd_, h, 12) &&
                                 !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4);
        while (ok) {
            if (!readAll(fd_, c, 8)) { ok = false; break; }
            uint32_t sz = get32(c + 4); off_t at = lseek(fd_, 0, SEEK_CUR);
            if (!memcmp(c, "data", 4)) {
                ok = fmtOk && at >= 0 && off_t(at) + off_t(sz) <= st.st_size;   // a truncated data chunk is refused
                frames_ = sz / 4;
                break;
            }
            if (!memcmp(c, "fmt ", 4)) {
                ok = sz >= 16 && readAll(fd_, f, 16) && get16(f) == 3 && get16(f + 2) == 1 && get16(f + 14) == 32;
                if (ok) { rate_ = get32(f + 4); fmtOk = true; }
            }
            if (ok) ok = at >= 0 && lseek(fd_, at + off_t(sz) + (sz & 1), SEEK_SET) >= 0;   // chunks are word-aligned
        }
        if (!ok) { ::close(fd_); fd_ = -1; }
        return ok;
#endif
    }
    uint32_t rate() const { return rate_; }
    uint32_t frames() const { return frames_; }
    bool read(float* s, uint32_t n) {
#ifdef _WIN32
        (void)s; (void)n; return false;
#else
        return fd_ >= 0 && readAll(fd_, s, size_t(n) * 4);
#endif
    }
private:
    int fd_ = -1;
    uint32_t rate_ = 0, frames_ = 0;
};

inline std::string formatPiece(const PieceMeta& m) {
    std::string s;
    char b[96];
    snprintf(b, sizeof b, "ouroboros-piece 1\nrate %u\nloop %u\nbars %u\nbpm %.3f\npart %d\nsong %d\nsteps",
             m.rate, m.loop, m.bars, double(m.bpm), m.part, m.song ? 1 : 0);
    s = b;
    for (int i = 0; i < kSteps; ++i) { snprintf(b, sizeof b, " %d:%u", m.stepPart[i], unsigned(m.stepReps[i])); s += b; }
    s += "\n";
    for (int p = 0; p < kParts; ++p)
        for (int t = 0; t < kTracks; ++t) {
            const PieceTrack& k = m.track[p][t];
            snprintf(b, sizeof b, "track %s %u %.2f %d\n", trackName(p, t).c_str(), k.len, double(k.volDb), k.on ? 1 : 0);
            s += b;
        }
    return s;
}

inline bool parsePiece(const std::string& text, PieceMeta& m) {
    std::istringstream in(text);
    std::string line;
    PieceMeta r;
    auto next = [&]() { return bool(std::getline(in, line)); };
    // A line with one value: the whole line must match.
    auto one = [&](const char* fmt, void* a) {
        int used = 0;
        return sscanf(line.c_str(), fmt, a, &used) == 1 && used == int(line.size());
    };
    unsigned u1, u2; int i1, i2; float f1; char nm[8];
    if (!next() || line != "ouroboros-piece 1") return false;
    if (!next() || !one("rate %u%n", &u1)) return false;
    r.rate = u1;
    if (!next() || !one("loop %u%n", &u1)) return false;
    r.loop = u1;
    if (!next() || !one("bars %u%n", &u1) || u1 < 1) return false;
    r.bars = u1;
    if (!next() || !one("bpm %f%n", &f1) || !(f1 >= 0.0f)) return false;
    r.bpm = f1;
    if (!next() || !one("part %d%n", &i1) || i1 < 0 || i1 >= kParts) return false;
    r.part = i1;
    if (!next() || !one("song %d%n", &i1) || (i1 != 0 && i1 != 1)) return false;
    r.song = i1 == 1;
    if (!next() || line.compare(0, 5, "steps") != 0) return false;
    const char* c = line.c_str() + 5;
    for (int i = 0; i < kSteps; ++i) {
        int used = 0;
        if (*c != ' ' || sscanf(c + 1, "%d:%d%n", &i1, &i2, &used) != 2) return false;
        if (i1 < 0 || i1 > kStepStop || i2 < 1 || i2 > 16) return false;
        r.stepPart[i] = int8_t(i1); r.stepReps[i] = uint8_t(i2);
        c += 1 + used;
    }
    if (*c) return false;
    for (int p = 0; p < kParts; ++p)
        for (int t = 0; t < kTracks; ++t) {
            PieceTrack& k = r.track[p][t];
            if (!next()) return false;
            int used = 0;
            if (sscanf(line.c_str(), "track %7s %u %f %d%n", nm, &u2, &f1, &i1, &used) != 4 || used != int(line.size())) return false;
            if (trackName(p, t) != nm || (i1 != 0 && i1 != 1)) return false;
            if (u2 && (r.loop == 0 || u2 % r.loop != 0)) return false;
            k.len = u2; k.volDb = f1; k.on = i1 == 1;
        }
    if (next() && !line.empty()) return false;
    m = r;
    return true;
}

inline void removeTree(const std::string& dir) {
#ifdef _WIN32
    (void)dir;
#else
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { unlink(dir.c_str()); return; }
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") removeTree(dir + "/" + n);
        }
        closedir(d);
    }
    rmdir(dir.c_str());
#endif
}

inline void recoverSlots(const std::string& root) {
#ifdef _WIN32
    (void)root;
#else
    for (int s = 1; s <= 16; ++s) {
        std::string d = slotDir(root, s), o = d + ".old";
        removeTree(d + ".new");
        if (!exists(d) && exists(o)) rename(o.c_str(), d.c_str());
        else if (exists(o)) removeTree(o);
    }
#endif
}

inline uint32_t scanSlots(const std::string& root) {
    uint32_t bits = 0;
#ifdef _WIN32
    (void)root;
#else
    for (int s = 1; s <= 16; ++s)
        if (exists(slotDir(root, s) + "/piece.txt")) bits |= 1u << (s - 1);
#endif
    return bits;
}

inline std::string stagingDir(const std::string& root, int slot) {
    std::string d = slotDir(root, slot) + ".new";
#ifndef _WIN32
    removeTree(d);
    makeDirs(d);
#endif
    return d;
}

inline Err commitSlot(const std::string& root, int slot) {
#ifdef _WIN32
    (void)root; (void)slot; return Err::Io;
#else
    std::string d = slotDir(root, slot), o = d + ".old", n = d + ".new";
    removeTree(o);
    bool had = exists(d);
    if (had && rename(d.c_str(), o.c_str()) != 0) return errFromErrno();
    if (rename(n.c_str(), d.c_str()) != 0) {
        Err e = errFromErrno();
        if (had) rename(o.c_str(), d.c_str());
        return e;
    }
    int fd = ::open(root.c_str(), O_RDONLY);   // the renames on disk before NN.old goes (else recoverSlots finds it)
    if (fd < 0) return errFromErrno();
    Err e = fsync(fd) != 0 ? errFromErrno() : Err::None;
    ::close(fd);
    if (e == Err::None) removeTree(o);
    return e;
#endif
}

} }
