/*
 * Copyright (C) 2024 USGS Astrogeology Science Center
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "cnet/net_protobuf.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cnet/proto_wire.hpp"

namespace cnet {

namespace {

// ISIS ControlNetVersioner byte layout constants (version 5 .net).
constexpr int kLabelBytes = 65536;  // fixed PVL label region at file start

// ---- Field numbers (from the ISIS .proto files) ---------------------------
// ControlNetFileHeaderV0005
namespace hdr {
enum { kNetworkId = 1, kTargetName = 2, kCreated = 3, kLastModified = 4,
       kDescription = 5, kUserName = 6, kNumPoints = 7, kTargetRadii = 10 };
}
// ControlPointFileEntryV0002 (point-level)
namespace pt {
enum { kId = 1, kType = 2, kChooserName = 3, kDatetime = 4, kEditLock = 5,
       kIgnore = 6, kJigsawRejected = 7, kReferenceIndex = 8,
       kAprioriSurfPointSource = 9, kAprioriSurfPointSourceFile = 10,
       kAprioriRadiusSource = 11, kAprioriRadiusSourceFile = 12,
       kLatitudeConstrained = 13, kLongitudeConstrained = 14, kRadiusConstrained = 15,
       kAprioriX = 16, kAprioriY = 17, kAprioriZ = 18, kAprioriCovar = 19,
       kAdjustedX = 20, kAdjustedY = 21, kAdjustedZ = 22, kAdjustedCovar = 23,
       kLog = 24, kMeasures = 25 };
}
// ControlPointFileEntryV0002.Measure
namespace ms {
enum { kSerialNumber = 1, kType = 2, kSample = 3, kLine = 4, kSampleResidual = 5,
       kLineResidual = 6, kChooserName = 7, kDatetime = 8, kEditLock = 9,
       kIgnore = 10, kJigsawRejected = 11, kDiameter = 12, kAprioriSample = 13,
       kAprioriLine = 14, kSampleSigma = 15, kLineSigma = 16, kLog = 17 };
}
// Measure.MeasureLogData
namespace mlog {
enum { kDoubleDataType = 1, kDoubleDataValue = 2, kBoolDataType = 3, kBoolDataValue = 4 };
}

/// Pull an integer value for `key` out of the PVL label region (first match).
/// The label is small ASCII PVL; a regex keyword scan is sufficient here.
int64_t pvlInt(const std::string& label, const char* key) {
    std::regex re(std::string("\\b") + key + R"(\s*=\s*(-?\d+))");
    std::smatch m;
    if (std::regex_search(label, m, re)) return std::stoll(m[1].str());
    throw std::runtime_error(std::string("control net label missing key: ") + key);
}

/// Decode one Measure submessage into the net's measure columns.
void readMeasure(wire::Reader r, ControlNet& net) {
    std::string serial, chooser, dt;
    int32_t type = 0;
    double sample = 0, line = 0, sres = 0, lres = 0, diam = 0, asamp = 0, aline = 0,
           ssig = 0, lsig = 0;
    uint8_t hSample = 0, hLine = 0, hSres = 0, hLres = 0, hDiam = 0, hAsamp = 0,
            hAline = 0, hSsig = 0, hLsig = 0, editLock = 0, ignore = 0, jig = 0,
            hEdit = 0, hIgn = 0, hJig = 0;
    std::vector<int32_t> logType;
    std::vector<double> logValue;

    uint32_t f, wt;
    while (r.readTag(f, wt)) {
        switch (f) {
            case ms::kSerialNumber: serial = r.readString(); break;
            case ms::kType: type = r.readInt32(); break;
            case ms::kSample: sample = r.readDouble(); hSample = 1; break;
            case ms::kLine: line = r.readDouble(); hLine = 1; break;
            case ms::kSampleResidual: sres = r.readDouble(); hSres = 1; break;
            case ms::kLineResidual: lres = r.readDouble(); hLres = 1; break;
            case ms::kChooserName: chooser = r.readString(); break;
            case ms::kDatetime: dt = r.readString(); break;
            case ms::kEditLock: editLock = r.readBool(); hEdit = 1; break;
            case ms::kIgnore: ignore = r.readBool(); hIgn = 1; break;
            case ms::kJigsawRejected: jig = r.readBool(); hJig = 1; break;
            case ms::kDiameter: diam = r.readDouble(); hDiam = 1; break;
            case ms::kAprioriSample: asamp = r.readDouble(); hAsamp = 1; break;
            case ms::kAprioriLine: aline = r.readDouble(); hAline = 1; break;
            case ms::kSampleSigma: ssig = r.readDouble(); hSsig = 1; break;
            case ms::kLineSigma: lsig = r.readDouble(); hLsig = 1; break;
            case ms::kLog: {
                wire::Reader lr = r.readSubMessage();
                int32_t dtype = 0;
                double dval = 0;
                uint32_t lf, lwt;
                while (lr.readTag(lf, lwt)) {
                    if (lf == mlog::kDoubleDataType) dtype = lr.readInt32();
                    else if (lf == mlog::kDoubleDataValue) dval = lr.readDouble();
                    else lr.skip(lwt);
                }
                logType.push_back(dtype);
                logValue.push_back(dval);
                break;
            }
            default: r.skip(wt); break;
        }
    }

    net.serialNumber.push_back(serial);
    net.measureType.push_back(static_cast<MeasureType>(type));
    net.sample.push_back(sample); net.hasSample.push_back(hSample);
    net.line.push_back(line); net.hasLine.push_back(hLine);
    net.sampleResidual.push_back(sres); net.hasSampleResidual.push_back(hSres);
    net.lineResidual.push_back(lres); net.hasLineResidual.push_back(hLres);
    net.measureChooserName.push_back(chooser);
    net.measureDatetime.push_back(dt);
    net.measureEditLock.push_back(editLock); net.hasMeasureEditLock.push_back(hEdit);
    net.measureIgnore.push_back(ignore); net.hasMeasureIgnore.push_back(hIgn);
    net.measureJigsawRejected.push_back(jig); net.hasMeasureJigsawRejected.push_back(hJig);
    net.diameter.push_back(diam); net.hasDiameter.push_back(hDiam);
    net.aprioriSample.push_back(asamp); net.hasAprioriSample.push_back(hAsamp);
    net.aprioriLine.push_back(aline); net.hasAprioriLine.push_back(hAline);
    net.sampleSigma.push_back(ssig); net.hasSampleSigma.push_back(hSsig);
    net.lineSigma.push_back(lsig); net.hasLineSigma.push_back(hLsig);

    for (size_t i = 0; i < logType.size(); ++i) {
        net.measureLogType.push_back(logType[i]);
        net.measureLogValue.push_back(logValue[i]);
    }
    net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
}

/// Decode one ControlPointFileEntryV0002 message into the net's point columns.
void readPoint(wire::Reader r, ControlNet& net) {
    std::string id, chooser, dt, aspSrcFile, arSrcFile;
    int32_t type = 0, refIndex = 0, aspSrc = 0, arSrc = 0;
    uint8_t editLock = 0, ignore = 0, jig = 0, hEdit = 0, hIgn = 0, hJig = 0, hRef = 0;
    double ax = 0, ay = 0, az = 0, dx = 0, dy = 0, dz = 0;
    uint8_t hApriori = 0, hAdjusted = 0;
    std::vector<double> aCovar, dCovar;
    uint32_t measuresBefore = static_cast<uint32_t>(net.numMeasures());
    uint32_t measureN = 0;

    uint32_t f, wt;
    while (r.readTag(f, wt)) {
        switch (f) {
            case pt::kId: id = r.readString(); break;
            case pt::kType: type = r.readInt32(); break;
            case pt::kChooserName: chooser = r.readString(); break;
            case pt::kDatetime: dt = r.readString(); break;
            case pt::kEditLock: editLock = r.readBool(); hEdit = 1; break;
            case pt::kIgnore: ignore = r.readBool(); hIgn = 1; break;
            case pt::kJigsawRejected: jig = r.readBool(); hJig = 1; break;
            case pt::kReferenceIndex: refIndex = r.readInt32(); hRef = 1; break;
            case pt::kAprioriSurfPointSource: aspSrc = r.readInt32(); break;
            case pt::kAprioriSurfPointSourceFile: aspSrcFile = r.readString(); break;
            case pt::kAprioriRadiusSource: arSrc = r.readInt32(); break;
            case pt::kAprioriRadiusSourceFile: arSrcFile = r.readString(); break;
            case pt::kAprioriX: ax = r.readDouble(); hApriori = 1; break;
            case pt::kAprioriY: ay = r.readDouble(); break;
            case pt::kAprioriZ: az = r.readDouble(); break;
            case pt::kAdjustedX: dx = r.readDouble(); hAdjusted = 1; break;
            case pt::kAdjustedY: dy = r.readDouble(); break;
            case pt::kAdjustedZ: dz = r.readDouble(); break;
            case pt::kAprioriCovar: {
                // packed repeated double
                wire::Reader sub = r.readSubMessage();
                while (!sub.done()) aCovar.push_back(sub.readDouble());
                break;
            }
            case pt::kAdjustedCovar: {
                wire::Reader sub = r.readSubMessage();
                while (!sub.done()) dCovar.push_back(sub.readDouble());
                break;
            }
            case pt::kMeasures: {
                readMeasure(r.readSubMessage(), net);
                ++measureN;
                break;
            }
            default: r.skip(wt); break;  // logs (24), constrained flags (13-15), etc.
        }
    }

    net.pointId.push_back(id);
    net.pointType.push_back(static_cast<PointType>(type));
    net.chooserName.push_back(chooser);
    net.datetime.push_back(dt);
    net.editLock.push_back(editLock); net.hasEditLock.push_back(hEdit);
    net.ignore.push_back(ignore); net.hasIgnore.push_back(hIgn);
    net.jigsawRejected.push_back(jig); net.hasJigsawRejected.push_back(hJig);
    net.referenceIndex.push_back(refIndex); net.hasReferenceIndex.push_back(hRef);
    net.aprioriSurfPointSource.push_back(static_cast<AprioriSource>(aspSrc));
    net.aprioriSurfPointSourceFile.push_back(aspSrcFile);
    net.aprioriRadiusSource.push_back(static_cast<AprioriSource>(arSrc));
    net.aprioriRadiusSourceFile.push_back(arSrcFile);
    net.aprioriX.push_back(ax); net.aprioriY.push_back(ay); net.aprioriZ.push_back(az);
    net.hasApriori.push_back(hApriori);
    net.adjustedX.push_back(dx); net.adjustedY.push_back(dy); net.adjustedZ.push_back(dz);
    net.hasAdjusted.push_back(hAdjusted);
    for (double v : aCovar) net.aprioriCovar.push_back(v);
    net.aprioriCovarOffset.push_back(static_cast<uint32_t>(net.aprioriCovar.size()));
    for (double v : dCovar) net.adjustedCovar.push_back(v);
    net.adjustedCovarOffset.push_back(static_cast<uint32_t>(net.adjustedCovar.size()));
    net.measureStart.push_back(measuresBefore);
    net.measureCount.push_back(measureN);
}

// Seed an empty net's CSR offset arrays (mirrors read_net_protobuf setup).
void seedCsr(ControlNet& net) {
    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);
}

// Parsed .net layout: byte offsets + (for V0005) per-point message sizes.
struct NetLayout {
    int64_t pointsStart = 0, pointsBytes = 0;
    std::vector<uint64_t> pointSizes;
    bool haveSizes = false;   // false => legacy inline-LE32-prefix framing
    NetworkHeader header;
};

// Parse the PVL label + header. `data`/`bufSize` must cover the label + header
// region; `fileSize` is the true file length used for the offset bounds check
// (the whole-file reader passes bufSize==fileSize; the streamer passes a small
// header buffer but the real file size).
NetLayout parseLayout(const uint8_t* data, size_t bufSize, size_t fileSize) {
    if (bufSize < static_cast<size_t>(kLabelBytes))
        throw std::runtime_error("control net file smaller than label region");
    std::string label(reinterpret_cast<const char*>(data), kLabelBytes);
    int64_t headerStart = pvlInt(label, "HeaderStartByte");
    int64_t headerBytes = pvlInt(label, "HeaderBytes");
    NetLayout L;
    L.pointsStart = pvlInt(label, "PointsStartByte");
    L.pointsBytes = pvlInt(label, "PointsBytes");
    if (headerStart < 0 || headerBytes < 0 || L.pointsStart < 0 || L.pointsBytes < 0 ||
        (uint64_t)(headerStart + headerBytes) > bufSize ||
        (uint64_t)(L.pointsStart + L.pointsBytes) > fileSize)
        throw std::runtime_error("control net byte offsets exceed file size");

    // Header field 7 length-delimited => V0005 packed pointMessageSizes; varint
    // => miniset-legacy numPoints (ignored; inline-prefix framing used instead).
    wire::Reader r(data + headerStart, static_cast<size_t>(headerBytes));
    uint32_t f, wt;
    while (r.readTag(f, wt)) {
        if (f == hdr::kNetworkId && wt == wire::kLenDelim) L.header.networkId = r.readString();
        else if (f == hdr::kTargetName && wt == wire::kLenDelim) L.header.targetName = r.readString();
        else if (f == hdr::kCreated && wt == wire::kLenDelim) L.header.created = r.readString();
        else if (f == hdr::kLastModified && wt == wire::kLenDelim) L.header.lastModified = r.readString();
        else if (f == hdr::kDescription && wt == wire::kLenDelim) L.header.description = r.readString();
        else if (f == hdr::kUserName && wt == wire::kLenDelim) L.header.userName = r.readString();
        else if (f == hdr::kNumPoints && wt == wire::kLenDelim) {
            wire::Reader sub = r.readSubMessage();
            L.haveSizes = true;
            while (!sub.done()) L.pointSizes.push_back(sub.readVarint());
        } else {
            r.skip(wt);
        }
    }
    return L;
}

// Decode points from the points block into `net`, invoking `perPoint` after each
// point is appended (lets the streamer flush a batch). V0005 uses header sizes;
// otherwise inline LE32 prefixes.
template <typename PerPoint>
void iteratePoints(const uint8_t* data, const NetLayout& L, ControlNet& net,
                   PerPoint&& perPoint) {
    const uint8_t* p = data + L.pointsStart;
    const uint8_t* end = data + L.pointsStart + L.pointsBytes;
    if (L.haveSizes) {
        for (uint64_t sz : L.pointSizes) {
            if (p + sz > end) throw std::runtime_error("V0005 point size exceeds points block");
            readPoint(wire::Reader(p, static_cast<size_t>(sz)), net);
            p += sz;
            perPoint();
        }
    } else {
        while (p + 4 <= end) {
            uint32_t sz; std::memcpy(&sz, p, 4); p += 4;
            if (p + sz > end) throw std::runtime_error("control point size exceeds points block");
            readPoint(wire::Reader(p, sz), net);
            p += sz;
            perPoint();
        }
    }
}

}  // namespace

ControlNet read_net_protobuf(const std::vector<uint8_t>& bytes) {
    NetLayout L = parseLayout(bytes.data(), bytes.size(), bytes.size());
    ControlNet net;
    net.header = L.header;
    seedCsr(net);
    if (L.haveSizes) net.pointId.reserve(L.pointSizes.size());
    iteratePoints(bytes.data(), L, net, []{});
    return net;
}

// ---- streaming reader ------------------------------------------------------
namespace {
// pread the full byte range [off, off+len) into buf (looped; handles >2 GB).
void preadExact(int fd, uint8_t* buf, size_t len, off_t off) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::pread(fd, buf + got, len - got, off + (off_t)got);
        if (n <= 0) throw std::runtime_error("control net: short pread");
        got += (size_t)n;
    }
}
}  // namespace

void stream_net_protobuf(const std::string& path, size_t batchPoints,
                         const std::function<void(ControlNet&)>& onBatch) {
    if (batchPoints == 0) batchPoints = 50000;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open control net: " + path);
    struct Closer { int fd; ~Closer(){ if(fd>=0) ::close(fd);} } closer{fd};

    // Read only the label+header region up front (small): enough to parse the
    // layout. Header end = pointsStart in ISIS layout, so read [0, pointsStart).
    // First read the label to learn the offsets, then the header if it extends
    // beyond the initial read.
    struct stat st{};
    if (::fstat(fd, &st) != 0) throw std::runtime_error("fstat failed: " + path);
    size_t fileSize = (size_t)st.st_size;

    // Read a generous prefix covering label + header. We don't know headerBytes
    // until we parse the label, so read the label first, then (re)read enough.
    std::vector<uint8_t> prefix(std::min<size_t>(fileSize, (size_t)kLabelBytes));
    preadExact(fd, prefix.data(), prefix.size(), 0);
    // Parse just the label to find where the header ends.
    // (parseLayout needs the header bytes present; grow prefix to cover it.)
    // The label region is kLabelBytes; HeaderStart/Bytes live in it.
    std::string label(reinterpret_cast<const char*>(prefix.data()),
                      std::min<size_t>(prefix.size(), (size_t)kLabelBytes));
    int64_t headerStart = pvlInt(label, "HeaderStartByte");
    int64_t headerBytes = pvlInt(label, "HeaderBytes");
    size_t headerEnd = (size_t)(headerStart + headerBytes);
    if (headerEnd > prefix.size()) {
        prefix.resize(headerEnd);
        preadExact(fd, prefix.data() + kLabelBytes, headerEnd - kLabelBytes, kLabelBytes);
    }
    NetLayout L = parseLayout(prefix.data(), prefix.size(), fileSize);

    ControlNet batch; batch.header = L.header; seedCsr(batch);
    size_t inBatch = 0;
    auto flush = [&]() {
        if (inBatch == 0) return;
        onBatch(batch);
        batch = ControlNet(); batch.header = L.header; seedCsr(batch);
        inBatch = 0;
    };

    // Stream the point block in bounded windows aligned to whole-point
    // boundaries, so resident memory is ~one window + one batch — independent of
    // file size. Points are decoded from the window buffer.
    const size_t kWindow = 64u * 1024u * 1024u;  // 64 MiB target window
    std::vector<uint8_t> win;
    off_t base = L.pointsStart;                  // file offset of current window
    off_t pend = L.pointsStart + L.pointsBytes;  // end of points block

    if (L.haveSizes) {
        // V0005: exact per-point sizes -> pack as many whole points as fit ~kWindow.
        size_t si = 0;  // index into pointSizes
        off_t cur = base;
        while (si < L.pointSizes.size()) {
            size_t winBytes = 0, winPts = 0;
            while (si + winPts < L.pointSizes.size()) {
                uint64_t sz = L.pointSizes[si + winPts];
                if (winBytes && winBytes + sz > kWindow) break;  // window full
                winBytes += sz; ++winPts;
            }
            if (winPts == 0) {  // single point larger than the window
                winBytes = L.pointSizes[si]; winPts = 1;
            }
            win.resize(winBytes);
            preadExact(fd, win.data(), winBytes, cur);
            const uint8_t* p = win.data();
            for (size_t k = 0; k < winPts; ++k) {
                uint64_t sz = L.pointSizes[si + k];
                readPoint(wire::Reader(p, (size_t)sz), batch);
                p += sz;
                if (++inBatch >= batchPoints) flush();
            }
            cur += (off_t)winBytes; si += winPts;
        }
    } else {
        // Legacy inline-LE32-prefix: read windows, parse whole points, and carry
        // over the trailing partial point to the next window.
        off_t cur = base;
        std::vector<uint8_t> carry;
        while (cur < pend) {
            size_t want = (size_t)std::min<off_t>((off_t)kWindow, pend - cur);
            size_t co = carry.size();
            win.resize(co + want);
            if (co) std::memcpy(win.data(), carry.data(), co);
            preadExact(fd, win.data() + co, want, cur);
            cur += (off_t)want;
            size_t pos = 0, avail = win.size();
            while (pos + 4 <= avail) {
                uint32_t sz; std::memcpy(&sz, win.data() + pos, 4);
                if (pos + 4 + sz > avail) break;  // partial point -> carry over
                readPoint(wire::Reader(win.data() + pos + 4, sz), batch);
                pos += 4 + sz;
                if (++inBatch >= batchPoints) flush();
            }
            carry.assign(win.begin() + pos, win.end());
        }
    }
    flush();
}

namespace {

/// Serialize one point (index i) to a ControlPointFileEntryV0002 message body.
std::vector<uint8_t> encodePoint(const ControlNet& net, size_t i) {
    std::vector<uint8_t> buf;
    wire::Writer w(buf);

    w.writeString(pt::kId, net.pointId[i]);
    w.writeEnum(pt::kType, static_cast<int32_t>(net.pointType[i]));
    if (!net.chooserName[i].empty()) w.writeString(pt::kChooserName, net.chooserName[i]);
    if (!net.datetime[i].empty()) w.writeString(pt::kDatetime, net.datetime[i]);
    if (net.hasEditLock[i]) w.writeBool(pt::kEditLock, net.editLock[i]);
    if (net.hasIgnore[i]) w.writeBool(pt::kIgnore, net.ignore[i]);
    if (net.hasJigsawRejected[i]) w.writeBool(pt::kJigsawRejected, net.jigsawRejected[i]);
    if (net.hasReferenceIndex[i]) w.writeInt32(pt::kReferenceIndex, net.referenceIndex[i]);
    w.writeEnum(pt::kAprioriSurfPointSource, static_cast<int32_t>(net.aprioriSurfPointSource[i]));
    if (!net.aprioriSurfPointSourceFile[i].empty())
        w.writeString(pt::kAprioriSurfPointSourceFile, net.aprioriSurfPointSourceFile[i]);
    w.writeEnum(pt::kAprioriRadiusSource, static_cast<int32_t>(net.aprioriRadiusSource[i]));
    if (!net.aprioriRadiusSourceFile[i].empty())
        w.writeString(pt::kAprioriRadiusSourceFile, net.aprioriRadiusSourceFile[i]);
    if (net.hasApriori[i]) {
        w.writeDouble(pt::kAprioriX, net.aprioriX[i]);
        w.writeDouble(pt::kAprioriY, net.aprioriY[i]);
        w.writeDouble(pt::kAprioriZ, net.aprioriZ[i]);
    }
    {
        uint32_t b = net.aprioriCovarOffset[i], e = net.aprioriCovarOffset[i + 1];
        if (e > b) w.writePackedDoubles(pt::kAprioriCovar, &net.aprioriCovar[b], e - b);
    }
    if (net.hasAdjusted[i]) {
        w.writeDouble(pt::kAdjustedX, net.adjustedX[i]);
        w.writeDouble(pt::kAdjustedY, net.adjustedY[i]);
        w.writeDouble(pt::kAdjustedZ, net.adjustedZ[i]);
    }
    {
        uint32_t b = net.adjustedCovarOffset[i], e = net.adjustedCovarOffset[i + 1];
        if (e > b) w.writePackedDoubles(pt::kAdjustedCovar, &net.adjustedCovar[b], e - b);
    }

    // Measures.
    uint32_t mstart = net.measureStart[i], mend = mstart + net.measureCount[i];
    for (uint32_t m = mstart; m < mend; ++m) {
        std::vector<uint8_t> mbuf;
        wire::Writer mw(mbuf);
        mw.writeString(ms::kSerialNumber, net.serialNumber[m]);
        mw.writeEnum(ms::kType, static_cast<int32_t>(net.measureType[m]));
        if (net.hasSample[m]) mw.writeDouble(ms::kSample, net.sample[m]);
        if (net.hasLine[m]) mw.writeDouble(ms::kLine, net.line[m]);
        if (net.hasSampleResidual[m]) mw.writeDouble(ms::kSampleResidual, net.sampleResidual[m]);
        if (net.hasLineResidual[m]) mw.writeDouble(ms::kLineResidual, net.lineResidual[m]);
        if (!net.measureChooserName[m].empty()) mw.writeString(ms::kChooserName, net.measureChooserName[m]);
        if (!net.measureDatetime[m].empty()) mw.writeString(ms::kDatetime, net.measureDatetime[m]);
        if (net.hasMeasureEditLock[m]) mw.writeBool(ms::kEditLock, net.measureEditLock[m]);
        if (net.hasMeasureIgnore[m]) mw.writeBool(ms::kIgnore, net.measureIgnore[m]);
        if (net.hasMeasureJigsawRejected[m]) mw.writeBool(ms::kJigsawRejected, net.measureJigsawRejected[m]);
        if (net.hasDiameter[m]) mw.writeDouble(ms::kDiameter, net.diameter[m]);
        if (net.hasAprioriSample[m]) mw.writeDouble(ms::kAprioriSample, net.aprioriSample[m]);
        if (net.hasAprioriLine[m]) mw.writeDouble(ms::kAprioriLine, net.aprioriLine[m]);
        if (net.hasSampleSigma[m]) mw.writeDouble(ms::kSampleSigma, net.sampleSigma[m]);
        if (net.hasLineSigma[m]) mw.writeDouble(ms::kLineSigma, net.lineSigma[m]);
        uint32_t lb = net.measureLogOffset[m], le = net.measureLogOffset[m + 1];
        for (uint32_t l = lb; l < le; ++l) {
            std::vector<uint8_t> logbuf;
            wire::Writer lw(logbuf);
            lw.writeInt32(mlog::kDoubleDataType, net.measureLogType[l]);
            lw.writeDouble(mlog::kDoubleDataValue, net.measureLogValue[l]);
            mw.writeBytes(ms::kLog, logbuf);
        }
        w.writeBytes(pt::kMeasures, mbuf);
    }
    return buf;
}

void appendLE32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

std::vector<uint8_t> write_net_protobuf(const ControlNet& net) {
    // Header message (ControlNetFileHeaderV0005).
    std::vector<uint8_t> headerBuf;
    {
        wire::Writer w(headerBuf);
        w.writeString(hdr::kNetworkId, net.header.networkId);
        w.writeString(hdr::kTargetName, net.header.targetName);
        if (!net.header.created.empty()) w.writeString(hdr::kCreated, net.header.created);
        if (!net.header.lastModified.empty()) w.writeString(hdr::kLastModified, net.header.lastModified);
        if (!net.header.description.empty()) w.writeString(hdr::kDescription, net.header.description);
        if (!net.header.userName.empty()) w.writeString(hdr::kUserName, net.header.userName);
        w.writeInt32(hdr::kNumPoints, static_cast<int32_t>(net.numPoints()));
    }

    // Points block.
    std::vector<uint8_t> pointsBuf;
    size_t numMeasures = 0;
    for (size_t i = 0; i < net.numPoints(); ++i) {
        std::vector<uint8_t> pb = encodePoint(net, i);
        appendLE32(pointsBuf, static_cast<uint32_t>(pb.size()));
        pointsBuf.insert(pointsBuf.end(), pb.begin(), pb.end());
        numMeasures += net.measureCount[i];
    }

    int64_t headerStart = kLabelBytes;
    int64_t headerBytes = static_cast<int64_t>(headerBuf.size());
    int64_t pointsStart = headerStart + headerBytes;
    int64_t pointsBytes = static_cast<int64_t>(pointsBuf.size());

    // PVL label describing the byte layout (matches ISIS ControlNetVersioner).
    std::string label =
        "Object = ProtoBuffer\n"
        "  Object = Core\n"
        "    HeaderStartByte = " + std::to_string(headerStart) + "\n"
        "    HeaderBytes     = " + std::to_string(headerBytes) + "\n"
        "    PointsStartByte = " + std::to_string(pointsStart) + "\n"
        "    PointsBytes     = " + std::to_string(pointsBytes) + "\n"
        "  End_Object\n\n"
        "  Group = ControlNetworkInfo\n"
        "    NetworkId  = " + net.header.networkId + "\n"
        "    TargetName = " + net.header.targetName + "\n"
        "    UserName   = " + net.header.userName + "\n"
        "    Created    = " + net.header.created + "\n"
        "    LastModified = " + net.header.lastModified + "\n"
        "    Description = " + net.header.description + "\n"
        "    NumberOfPoints = " + std::to_string(net.numPoints()) + "\n"
        "    NumberOfMeasures = " + std::to_string(numMeasures) + "\n"
        "    Version = 5\n"
        "  End_Group\n"
        "End_Object\n"
        "End\n";
    if (static_cast<int>(label.size()) >= kLabelBytes) {
        throw std::runtime_error("control net label exceeds reserved region");
    }

    std::vector<uint8_t> out(kLabelBytes, 0);
    std::memcpy(out.data(), label.data(), label.size());
    out.insert(out.end(), headerBuf.begin(), headerBuf.end());
    out.insert(out.end(), pointsBuf.begin(), pointsBuf.end());
    return out;
}

}  // namespace cnet
