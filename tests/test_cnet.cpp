// Control-network tests. Everything runs through /vsimem — no files on disk.
// A field-diverse network (mirroring ISIS's Parquet round-trip test) is built in
// memory, then round-tripped through both the protobuf .net and Parquet writers,
// and the two formats are checked for parity.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <cpl_vsi.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <gtest/gtest.h>

#include "cnet/column_source.hpp"
#include "cnet/control_net.hpp"
#include "cnet/control_net_io.hpp"
#include "cnet/control_net_reader.hpp"
#include "cnet/miniparquet_io.hpp"
#include "cnet/net_protobuf.hpp"
#include "cnet/stards_io.hpp"

#include "miniparquet.hpp"

using namespace cnet;

namespace {

// Build the same field-diverse net ISIS uses: a Free point with apriori +
// adjusted surface points (with covariance) and two measures (one ignored, one
// fully populated with residuals/sigmas/log), a Constrained point with one
// measure, and a Fixed point with no measures (zero-measure row path).
ControlNet buildTestNetwork() {
    ControlNet net;
    net.header.networkId = "TestParquetNet";
    net.header.targetName = "Mars";
    net.header.userName = "tester";
    net.header.description = "Parquet round-trip test network";

    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);

    auto addMeasure = [&](const std::string& sn, MeasureType t, bool hasSampLine,
                          double s, double l, bool ignored, bool hasResid,
                          double sr, double lr, int logType, double logVal) {
        net.serialNumber.push_back(sn);
        net.measureType.push_back(t);
        net.sample.push_back(s); net.hasSample.push_back(hasSampLine);
        net.line.push_back(l); net.hasLine.push_back(hasSampLine);
        net.sampleResidual.push_back(sr); net.hasSampleResidual.push_back(hasResid);
        net.lineResidual.push_back(lr); net.hasLineResidual.push_back(hasResid);
        net.measureChooserName.push_back("");
        net.measureDatetime.push_back("");
        net.measureEditLock.push_back(0); net.hasMeasureEditLock.push_back(0);
        net.measureIgnore.push_back(ignored ? 1 : 0); net.hasMeasureIgnore.push_back(ignored ? 1 : 0);
        net.measureJigsawRejected.push_back(0); net.hasMeasureJigsawRejected.push_back(0);
        net.diameter.push_back(0); net.hasDiameter.push_back(0);
        net.aprioriSample.push_back(0); net.hasAprioriSample.push_back(0);
        net.aprioriLine.push_back(0); net.hasAprioriLine.push_back(0);
        net.sampleSigma.push_back(0); net.hasSampleSigma.push_back(0);
        net.lineSigma.push_back(0); net.hasLineSigma.push_back(0);
        if (logType >= 0) { net.measureLogType.push_back(logType); net.measureLogValue.push_back(logVal); }
        net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
    };

    auto startPoint = [&](const std::string& id, PointType type) {
        net.pointId.push_back(id);
        net.pointType.push_back(type);
        net.chooserName.push_back("");
        net.datetime.push_back("");
        net.editLock.push_back(0); net.hasEditLock.push_back(0);
        net.ignore.push_back(0); net.hasIgnore.push_back(0);
        net.jigsawRejected.push_back(0); net.hasJigsawRejected.push_back(0);
        net.referenceIndex.push_back(0); net.hasReferenceIndex.push_back(0);
        net.aprioriSurfPointSource.push_back(AprioriSource::None);
        net.aprioriSurfPointSourceFile.push_back("");
        net.aprioriRadiusSource.push_back(AprioriSource::None);
        net.aprioriRadiusSourceFile.push_back("");
        net.aprioriX.push_back(0); net.aprioriY.push_back(0); net.aprioriZ.push_back(0); net.hasApriori.push_back(0);
        net.adjustedX.push_back(0); net.adjustedY.push_back(0); net.adjustedZ.push_back(0); net.hasAdjusted.push_back(0);
        net.aprioriCovarOffset.push_back(static_cast<uint32_t>(net.aprioriCovar.size()));
        net.adjustedCovarOffset.push_back(static_cast<uint32_t>(net.adjustedCovar.size()));
        net.measureStart.push_back(static_cast<uint32_t>(net.numMeasures()));
        net.measureCount.push_back(0);
    };

    // p1: Free, apriori+adjusted+covariance, 2 measures.
    startPoint("p1", PointType::Free);
    net.aprioriSurfPointSource.back() = AprioriSource::Reference;
    net.aprioriRadiusSource.back() = AprioriSource::DEM;
    net.aprioriX.back() = 1000.0; net.aprioriY.back() = 2000.0; net.aprioriZ.back() = 3000.0; net.hasApriori.back() = 1;
    net.adjustedX.back() = 1001.0; net.adjustedY.back() = 2002.0; net.adjustedZ.back() = 3003.0; net.hasAdjusted.back() = 1;
    for (double v : {100.0, 1.0, 2.0, 200.0, 3.0, 300.0}) net.aprioriCovar.push_back(v);
    net.aprioriCovarOffset.back() = static_cast<uint32_t>(net.aprioriCovar.size());
    for (double v : {100.0, 1.0, 2.0, 200.0, 3.0, 300.0}) net.adjustedCovar.push_back(v);
    net.adjustedCovarOffset.back() = static_cast<uint32_t>(net.adjustedCovar.size());
    addMeasure("SN_A", MeasureType::RegisteredSubPixel, true, 10.5, 20.5, false, true, 0.1, 0.2, 2, 0.95);
    net.measureCount.back()++;
    addMeasure("SN_B", MeasureType::Candidate, true, 30.0, 40.0, true, false, 0, 0, -1, 0);
    net.measureCount.back()++;

    // p2: Constrained, one measure.
    startPoint("p2", PointType::Constrained);
    addMeasure("SN_A", MeasureType::Manual, true, 50.0, 60.0, false, false, 0, 0, -1, 0);
    net.measureCount.back()++;

    // p3: Fixed, zero measures.
    startPoint("p3", PointType::Fixed);

    return net;
}

void expectEqual(const ControlNet& a, const ControlNet& b) {
    ASSERT_EQ(a.numPoints(), b.numPoints());
    ASSERT_EQ(a.numMeasures(), b.numMeasures());
    EXPECT_EQ(a.header.networkId, b.header.networkId);
    EXPECT_EQ(a.header.targetName, b.header.targetName);
    EXPECT_EQ(a.header.userName, b.header.userName);

    for (size_t i = 0; i < a.numPoints(); ++i) {
        EXPECT_EQ(a.pointId[i], b.pointId[i]);
        EXPECT_EQ(a.pointType[i], b.pointType[i]);
        EXPECT_EQ(a.measureCount[i], b.measureCount[i]);
        EXPECT_EQ(a.hasApriori[i], b.hasApriori[i]);
        if (a.hasApriori[i]) {
            EXPECT_DOUBLE_EQ(a.aprioriX[i], b.aprioriX[i]);
            EXPECT_DOUBLE_EQ(a.aprioriY[i], b.aprioriY[i]);
            EXPECT_DOUBLE_EQ(a.aprioriZ[i], b.aprioriZ[i]);
        }
        // Covariance count parity.
        EXPECT_EQ(a.aprioriCovarOffset[i + 1] - a.aprioriCovarOffset[i],
                  b.aprioriCovarOffset[i + 1] - b.aprioriCovarOffset[i]);
    }
    for (size_t m = 0; m < a.numMeasures(); ++m) {
        EXPECT_EQ(a.serialNumber[m], b.serialNumber[m]);
        EXPECT_EQ(a.measureType[m], b.measureType[m]);
        EXPECT_EQ(a.hasSample[m], b.hasSample[m]);
        if (a.hasSample[m]) EXPECT_DOUBLE_EQ(a.sample[m], b.sample[m]);
        if (a.hasLine[m]) EXPECT_DOUBLE_EQ(a.line[m], b.line[m]);
    }
}

}  // namespace

TEST(Cnet, ProtobufRoundTrip) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/rt.net");
    ControlNet back = read_control_net("/vsimem/rt.net");
    expectEqual(net, back);
    VSIUnlink("/vsimem/rt.net");
}

// The streaming .net reader (bounded memory) reassembles to the same net as a
// whole-file read. Small batch size forces multiple flushes across point runs.
TEST(Cnet, StreamNetProtobufReassembles) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "stream.net";
    write_control_net(net, path);  // miniset-legacy inline-prefix framing

    // Concatenate streamed batches back into one net and compare.
    ControlNet acc; acc.aprioriCovarOffset.clear(); acc.adjustedCovarOffset.clear();
    acc.measureLogOffset.clear();
    bool first = true;
    size_t batches = 0, pts = 0;
    stream_net_protobuf(path, 1 /*one point per batch*/, [&](ControlNet& b) {
        ++batches; pts += b.numPoints();
        if (first) { acc = b; first = false; }
        else {
            // append batch b's points+measures to acc, fixing CSR offsets.
            uint32_t measBase = static_cast<uint32_t>(acc.numMeasures());
            uint32_t aprBase = static_cast<uint32_t>(acc.aprioriCovar.size());
            uint32_t adjBase = static_cast<uint32_t>(acc.adjustedCovar.size());
            uint32_t logBase = static_cast<uint32_t>(acc.measureLogType.size());
            for (size_t i = 0; i < b.numPoints(); ++i) {
                acc.pointId.push_back(b.pointId[i]); acc.pointType.push_back(b.pointType[i]);
                acc.chooserName.push_back(b.chooserName[i]); acc.datetime.push_back(b.datetime[i]);
                acc.editLock.push_back(b.editLock[i]); acc.hasEditLock.push_back(b.hasEditLock[i]);
                acc.ignore.push_back(b.ignore[i]); acc.hasIgnore.push_back(b.hasIgnore[i]);
                acc.jigsawRejected.push_back(b.jigsawRejected[i]); acc.hasJigsawRejected.push_back(b.hasJigsawRejected[i]);
                acc.referenceIndex.push_back(b.referenceIndex[i]); acc.hasReferenceIndex.push_back(b.hasReferenceIndex[i]);
                acc.aprioriSurfPointSource.push_back(b.aprioriSurfPointSource[i]);
                acc.aprioriSurfPointSourceFile.push_back(b.aprioriSurfPointSourceFile[i]);
                acc.aprioriRadiusSource.push_back(b.aprioriRadiusSource[i]);
                acc.aprioriRadiusSourceFile.push_back(b.aprioriRadiusSourceFile[i]);
                acc.aprioriX.push_back(b.aprioriX[i]); acc.aprioriY.push_back(b.aprioriY[i]);
                acc.aprioriZ.push_back(b.aprioriZ[i]); acc.hasApriori.push_back(b.hasApriori[i]);
                acc.adjustedX.push_back(b.adjustedX[i]); acc.adjustedY.push_back(b.adjustedY[i]);
                acc.adjustedZ.push_back(b.adjustedZ[i]); acc.hasAdjusted.push_back(b.hasAdjusted[i]);
                acc.measureStart.push_back(measBase + b.measureStart[i]);
                acc.measureCount.push_back(b.measureCount[i]);
                acc.aprioriCovarOffset.push_back(aprBase + b.aprioriCovarOffset[i + 1]);
                acc.adjustedCovarOffset.push_back(adjBase + b.adjustedCovarOffset[i + 1]);
            }
            for (double v : b.aprioriCovar) acc.aprioriCovar.push_back(v);
            for (double v : b.adjustedCovar) acc.adjustedCovar.push_back(v);
            for (size_t m = 0; m < b.numMeasures(); ++m) {
                acc.serialNumber.push_back(b.serialNumber[m]); acc.measureType.push_back(b.measureType[m]);
                acc.sample.push_back(b.sample[m]); acc.hasSample.push_back(b.hasSample[m]);
                acc.line.push_back(b.line[m]); acc.hasLine.push_back(b.hasLine[m]);
                acc.sampleResidual.push_back(b.sampleResidual[m]); acc.hasSampleResidual.push_back(b.hasSampleResidual[m]);
                acc.lineResidual.push_back(b.lineResidual[m]); acc.hasLineResidual.push_back(b.hasLineResidual[m]);
                acc.measureChooserName.push_back(b.measureChooserName[m]); acc.measureDatetime.push_back(b.measureDatetime[m]);
                acc.measureEditLock.push_back(b.measureEditLock[m]); acc.hasMeasureEditLock.push_back(b.hasMeasureEditLock[m]);
                acc.measureIgnore.push_back(b.measureIgnore[m]); acc.hasMeasureIgnore.push_back(b.hasMeasureIgnore[m]);
                acc.measureJigsawRejected.push_back(b.measureJigsawRejected[m]); acc.hasMeasureJigsawRejected.push_back(b.hasMeasureJigsawRejected[m]);
                acc.diameter.push_back(b.diameter[m]); acc.hasDiameter.push_back(b.hasDiameter[m]);
                acc.aprioriSample.push_back(b.aprioriSample[m]); acc.hasAprioriSample.push_back(b.hasAprioriSample[m]);
                acc.aprioriLine.push_back(b.aprioriLine[m]); acc.hasAprioriLine.push_back(b.hasAprioriLine[m]);
                acc.sampleSigma.push_back(b.sampleSigma[m]); acc.hasSampleSigma.push_back(b.hasSampleSigma[m]);
                acc.lineSigma.push_back(b.lineSigma[m]); acc.hasLineSigma.push_back(b.hasLineSigma[m]);
                acc.measureLogOffset.push_back(logBase + b.measureLogOffset[m + 1]);
            }
            for (int32_t v : b.measureLogType) acc.measureLogType.push_back(v);
            for (double v : b.measureLogValue) acc.measureLogValue.push_back(v);
        }
    });
    EXPECT_EQ(batches, net.numPoints());  // 1 point/batch
    EXPECT_EQ(pts, net.numPoints());
    expectEqual(net, acc);
    std::remove(path.c_str());
}

TEST(Cnet, ParquetRoundTrip) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/rt.parquet");
    ControlNet back = read_control_net("/vsimem/rt.parquet");
    expectEqual(net, back);
    VSIUnlink("/vsimem/rt.parquet");
}

TEST(Cnet, ProtobufParquetParity) {
    // The same net written to both formats must read back identically.
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/a.net");
    write_control_net(net, "/vsimem/a.parquet");
    ControlNet fromNet = read_control_net("/vsimem/a.net");
    ControlNet fromParquet = read_control_net("/vsimem/a.parquet");
    expectEqual(fromNet, fromParquet);
    VSIUnlink("/vsimem/a.net");
    VSIUnlink("/vsimem/a.parquet");
}

// Exercises the WASM ingest path (control_net_from_columns) natively: build a
// denormalized ColumnBatch (one row per measure, as a JS Parquet reader would
// hand over) and assert the assembled net matches the original.
TEST(Cnet, ColumnBatchIngestMatchesModel) {
    ControlNet net = buildTestNetwork();

    ColumnBatch b;
    b.networkId = net.header.networkId;
    b.targetName = net.header.targetName;
    b.userName = net.header.userName;
    b.description = net.header.description;

    // Emit one row per measure (and one sentinel row for zero-measure points),
    // repeating point + covariance columns on the point's first row only — the
    // ingest advances covariance/log cursors per row exactly as here.
    for (size_t p = 0; p < net.numPoints(); ++p) {
        uint32_t mstart = net.measureStart[p], mcount = net.measureCount[p];
        auto pushPointCols = [&](bool first) {
            b.id.push_back(net.pointId[p]);
            b.type.push_back(static_cast<int32_t>(net.pointType[p]));
            b.chooserName.push_back(net.chooserName[p]);
            b.datetime.push_back(net.datetime[p]);
            b.aprioriSurfPointSource.push_back(static_cast<int32_t>(net.aprioriSurfPointSource[p]));
            b.aprioriRadiusSource.push_back(static_cast<int32_t>(net.aprioriRadiusSource[p]));
            b.aprioriSurfPointSourceFile.push_back(net.aprioriSurfPointSourceFile[p]);
            b.aprioriRadiusSourceFile.push_back(net.aprioriRadiusSourceFile[p]);
            b.aprioriX.push_back(net.aprioriX[p]); b.aprioriY.push_back(net.aprioriY[p]); b.aprioriZ.push_back(net.aprioriZ[p]);
            b.hasApriori.push_back(net.hasApriori[p]);
            b.adjustedX.push_back(net.adjustedX[p]); b.adjustedY.push_back(net.adjustedY[p]); b.adjustedZ.push_back(net.adjustedZ[p]);
            b.hasAdjusted.push_back(net.hasAdjusted[p]);
            // Covariances only on the first row of the point.
            uint32_t ab = net.aprioriCovarOffset[p], ae = net.aprioriCovarOffset[p + 1];
            uint32_t db = net.adjustedCovarOffset[p], de = net.adjustedCovarOffset[p + 1];
            if (first) {
                for (uint32_t k = ab; k < ae; ++k) b.aprioriCovar.push_back(net.aprioriCovar[k]);
                b.aprioriCovarLen.push_back(static_cast<int32_t>(ae - ab));
                for (uint32_t k = db; k < de; ++k) b.adjustedCovar.push_back(net.adjustedCovar[k]);
                b.adjustedCovarLen.push_back(static_cast<int32_t>(de - db));
            } else {
                b.aprioriCovarLen.push_back(0);
                b.adjustedCovarLen.push_back(0);
            }
        };
        auto pushMeasure = [&](uint32_t m) {
            b.serialnumber.push_back(net.serialNumber[m]);
            b.measureType.push_back(static_cast<int32_t>(net.measureType[m]));
            b.sample.push_back(net.sample[m]); b.hasSample.push_back(net.hasSample[m]);
            b.line.push_back(net.line[m]); b.hasLine.push_back(net.hasLine[m]);
            uint32_t lb = net.measureLogOffset[m], le = net.measureLogOffset[m + 1];
            for (uint32_t k = lb; k < le; ++k) { b.measureLogType.push_back(net.measureLogType[k]); b.measureLogValue.push_back(net.measureLogValue[k]); }
            b.measureLogLen.push_back(static_cast<int32_t>(le - lb));
        };
        auto pushEmptyMeasure = [&]() {
            b.serialnumber.push_back("");
            b.measureType.push_back(0);
            b.sample.push_back(0); b.hasSample.push_back(0);
            b.line.push_back(0); b.hasLine.push_back(0);
            b.measureLogLen.push_back(0);
        };
        if (mcount == 0) {
            pushPointCols(true);
            pushEmptyMeasure();
        } else {
            for (uint32_t m = 0; m < mcount; ++m) {
                pushPointCols(m == 0);
                pushMeasure(mstart + m);
            }
        }
    }

    ControlNet assembled = control_net_from_columns(b);
    expectEqual(net, assembled);
}

// Lazy reader: open reads shape only; slices match the eager full read.
TEST(CnetReader, LazyShapeAndSlices) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/lazy.parquet");

    ControlNetReader r = ControlNetReader::open("/vsimem/lazy.parquet");
    EXPECT_TRUE(r.supportsLazy());
    ASSERT_EQ(r.numPoints(), net.numPoints());       // 3 points
    // Rows = 3 measures + 1 sentinel row for the zero-measure point p3 = 4.
    EXPECT_EQ(r.numRows(), net.numMeasures() + 1);
    EXPECT_EQ(r.header().networkId, net.header.networkId);
    EXPECT_EQ(r.pointId(0), "p1");
    EXPECT_EQ(r.pointId(2), "p3");

    // Point 0 (p1) owns 2 measure rows starting at row 0.
    uint32_t first, count;
    r.measureRange(0, first, count);
    EXPECT_EQ(first, 0u);
    EXPECT_EQ(count, 2u);

    // readPoints(0,1) returns exactly p1 with both measures.
    ControlNet slice = r.readPoints(0, 1);
    ASSERT_EQ(slice.numPoints(), 1u);
    EXPECT_EQ(slice.pointId[0], "p1");
    EXPECT_EQ(slice.measureCount[0], 2u);
    EXPECT_EQ(slice.numMeasures(), 2u);

    // readPoints(1,2) returns p2 (1 measure) and p3 (0 measures).
    ControlNet tail = r.readPoints(1, 2);
    ASSERT_EQ(tail.numPoints(), 2u);
    EXPECT_EQ(tail.pointId[0], "p2");
    EXPECT_EQ(tail.pointId[1], "p3");
    EXPECT_EQ(tail.measureCount[1], 0u);

    VSIUnlink("/vsimem/lazy.parquet");
}

// Column projection: request a subset; unrequested columns come back unset.
TEST(CnetReader, ColumnProjection) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/proj.parquet");

    ControlNetReader r = ControlNetReader::open("/vsimem/proj.parquet");
    ReadOptions opts;
    opts.columns = {"id", "type", "serialnumber", "measure_type", "sample", "line"};
    ControlNet slice = r.readPoints(0, 1, opts);

    ASSERT_EQ(slice.numPoints(), 1u);
    EXPECT_EQ(slice.pointId[0], "p1");
    // Requested measure fields are present...
    ASSERT_EQ(slice.numMeasures(), 2u);
    EXPECT_TRUE(slice.hasSample[0]);
    // ...while covariance columns (not requested) were not read: no covar values.
    EXPECT_EQ(slice.aprioriCovar.size(), 0u);
    EXPECT_FALSE(slice.hasApriori[0]);
    VSIUnlink("/vsimem/proj.parquet");
}

// A lazily-read slice equals the same points from an eager full read.
TEST(CnetReader, SliceMatchesEager) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/slice.parquet");

    ControlNet full = read_control_net("/vsimem/slice.parquet");
    ControlNetReader r = ControlNetReader::open("/vsimem/slice.parquet");
    ControlNet p1 = r.readPoints(0, 1);

    // p1's fields match the eager read's point 0.
    EXPECT_EQ(p1.pointId[0], full.pointId[0]);
    EXPECT_EQ(p1.pointType[0], full.pointType[0]);
    EXPECT_EQ(p1.measureCount[0], full.measureCount[0]);
    ASSERT_EQ(p1.numMeasures(), full.measureCount[0]);
    for (uint32_t m = 0; m < p1.numMeasures(); ++m) {
        EXPECT_EQ(p1.serialNumber[m], full.serialNumber[full.measureStart[0] + m]);
    }
    VSIUnlink("/vsimem/slice.parquet");
}

// readAll() via the lazy reader equals the eager reader.
TEST(CnetReader, ReadAllEqualsEager) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/all.parquet");
    ControlNet eager = read_control_net("/vsimem/all.parquet");
    ControlNet viaReader = ControlNetReader::open("/vsimem/all.parquet").readAll();
    expectEqual(eager, viaReader);
    VSIUnlink("/vsimem/all.parquet");
}

// .net is not lazily seekable but still opens and slices from the parsed net.
TEST(CnetReader, ProtobufNotLazyButUsable) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/r.net");
    ControlNetReader r = ControlNetReader::open("/vsimem/r.net");
    EXPECT_FALSE(r.supportsLazy());
    EXPECT_EQ(r.numPoints(), net.numPoints());
    EXPECT_EQ(r.pointId(0), "p1");
    VSIUnlink("/vsimem/r.net");
}

TEST(Cnet, FormatInference) {
    EXPECT_EQ(infer_net_format("x.parquet"), NetFormat::Parquet);
    EXPECT_EQ(infer_net_format("x.PARQUET"), NetFormat::Parquet);
    EXPECT_EQ(infer_net_format("/vsis3/b/x.parquet?list=no"), NetFormat::Parquet);
    EXPECT_EQ(infer_net_format("x.stards"), NetFormat::StarDS);
    EXPECT_EQ(infer_net_format("x.STARDS"), NetFormat::StarDS);
    EXPECT_EQ(infer_net_format("/vsis3/b/x.stards?list=no"), NetFormat::StarDS);
    EXPECT_EQ(infer_net_format("x.net"), NetFormat::Protobuf);
    EXPECT_EQ(infer_net_format("x.cnet"), NetFormat::Protobuf);
}

// StarDS is a real-filesystem columnar store (no /vsimem), so these round-trip
// through a temp file. Each ControlNet column becomes a named StarDS array; the
// full SoA model must survive a write/read cycle.
TEST(Cnet, StardsRoundTrip) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "rt.stards";
    write_control_net(net, path);
    ControlNet back = read_control_net(path);
    expectEqual(net, back);
    std::remove(path.c_str());
}

// Written to both StarDS and protobuf, the same net must read back identically.
TEST(Cnet, StardsProtobufParity) {
    ControlNet net = buildTestNetwork();
    std::string spath = std::string(::testing::TempDir()) + "parity.stards";
    write_control_net(net, spath);
    write_control_net(net, "/vsimem/parity.net");
    ControlNet fromStards = read_control_net(spath);
    ControlNet fromNet = read_control_net("/vsimem/parity.net");
    expectEqual(fromNet, fromStards);
    std::remove(spath.c_str());
    VSIUnlink("/vsimem/parity.net");
}

// Deep round-trip: covariance/log CSR arrays and full measure fields survive.
TEST(Cnet, StardsDeepFields) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "deep.stards";
    write_control_net(net, path);
    ControlNet back = read_control_net(path);

    ASSERT_EQ(back.aprioriCovar.size(), net.aprioriCovar.size());
    for (size_t i = 0; i < net.aprioriCovar.size(); ++i)
        EXPECT_DOUBLE_EQ(back.aprioriCovar[i], net.aprioriCovar[i]);
    ASSERT_EQ(back.aprioriCovarOffset, net.aprioriCovarOffset);
    ASSERT_EQ(back.adjustedCovarOffset, net.adjustedCovarOffset);

    ASSERT_EQ(back.measureLogType, net.measureLogType);
    ASSERT_EQ(back.measureLogValue.size(), net.measureLogValue.size());
    for (size_t i = 0; i < net.measureLogValue.size(); ++i)
        EXPECT_DOUBLE_EQ(back.measureLogValue[i], net.measureLogValue[i]);
    ASSERT_EQ(back.measureLogOffset, net.measureLogOffset);

    // Residuals + sigmas + apriori-source enums round-trip.
    ASSERT_EQ(back.sampleResidual, net.sampleResidual);
    ASSERT_EQ(back.aprioriSurfPointSource, net.aprioriSurfPointSource);
    ASSERT_EQ(back.aprioriRadiusSource, net.aprioriRadiusSource);
    EXPECT_EQ(back.header.description, net.header.description);

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Gaussian-splat LOD summary ("cnet/3")
// ---------------------------------------------------------------------------

namespace {

// Build a net whose adjusted points form `nClusters` elongated ribbons (one
// splat should capture each). Points are laid out cluster-by-cluster so file
// order is already contiguous per cluster — matching how the segment fitter
// partitions. `perCluster` points each. Deterministic (no RNG).
ControlNet buildRibbonNetwork(int nClusters, int perCluster) {
    ControlNet net;
    net.header.networkId = "RibbonNet";
    net.header.targetName = "Mars";
    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);

    // Cluster centers spread around the globe; each ribbon is long along one
    // body axis (~2e5 m) and thin across (~2e3 m) — highly anisotropic.
    const double R = 3.4e6;
    for (int c = 0; c < nClusters; ++c) {
        double cx = R * std::cos(c), cy = R * std::sin(c), cz = 1.0e5 * c;
        for (int i = 0; i < perCluster; ++i) {
            double t = (static_cast<double>(i) / perCluster - 0.5) * 2.0e5;  // long axis
            double w = ((i % 7) - 3) * 5.0e2;                                // thin axis
            net.pointId.push_back("p" + std::to_string(c) + "_" + std::to_string(i));
            net.pointType.push_back(PointType::Free);
            net.chooserName.push_back(""); net.datetime.push_back("");
            net.editLock.push_back(0); net.hasEditLock.push_back(0);
            net.ignore.push_back(0); net.hasIgnore.push_back(0);
            net.jigsawRejected.push_back(0); net.hasJigsawRejected.push_back(0);
            net.referenceIndex.push_back(0); net.hasReferenceIndex.push_back(0);
            net.aprioriSurfPointSource.push_back(AprioriSource::None);
            net.aprioriSurfPointSourceFile.push_back("");
            net.aprioriRadiusSource.push_back(AprioriSource::None);
            net.aprioriRadiusSourceFile.push_back("");
            net.aprioriX.push_back(0); net.aprioriY.push_back(0); net.aprioriZ.push_back(0);
            net.hasApriori.push_back(0);
            net.adjustedX.push_back(cx + t); net.adjustedY.push_back(cy + w);
            net.adjustedZ.push_back(cz); net.hasAdjusted.push_back(1);
            net.aprioriCovarOffset.push_back(static_cast<uint32_t>(net.aprioriCovar.size()));
            net.adjustedCovarOffset.push_back(static_cast<uint32_t>(net.adjustedCovar.size()));
            net.measureStart.push_back(static_cast<uint32_t>(net.numMeasures()));
            net.measureCount.push_back(0);
        }
    }
    return net;
}

}  // namespace

// The K splat ranges must be an exact contiguous partition of the points, and
// their weights must sum to the number of contributing (adjusted) points.
TEST(StardsSummary, CoveragePartition) {
    ControlNet net = buildRibbonNetwork(4, 500);  // 2000 points
    GaussianSummary s = fit_gaussian_summary(net, 4);
    ASSERT_EQ(s.size(), 4u);

    uint64_t wsum = 0, rcsum = 0;
    uint32_t expectStart = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        EXPECT_EQ(s.rangeStart[i], expectStart);  // contiguous, no gaps/overlaps
        expectStart += s.rangeCount[i];
        wsum += s.weight[i];
        rcsum += s.rangeCount[i];
    }
    EXPECT_EQ(rcsum, net.numPoints());   // ranges tile all points
    EXPECT_EQ(wsum, net.numPoints());    // all points are adjusted here
    EXPECT_EQ(expectStart, net.numPoints());
}

// Drill-down: reading a splat's [rangeStart, rangeCount) window back must return
// exactly that cluster's points, and their empirical mean must match the stored
// mu. This is the core correctness guarantee of the format.
TEST(StardsSummary, DrillDownMatchesMu) {
    ControlNet net = buildRibbonNetwork(4, 500);
    std::string path = std::string(::testing::TempDir()) + "summary.stards";
    write_control_net_stards_summarized(net, path, 4);

    SummaryReader sr = SummaryReader::open(path);
    ASSERT_EQ(sr.count(), 4u);
    const GaussianSummary& s = sr.splats();

    HeroPointsReader hero = HeroPointsReader::open(path);
    ASSERT_EQ(hero.count(), net.numPoints());

    for (size_t i = 0; i < s.size(); ++i) {
        std::vector<float> xyz;
        double radius = 0;
        size_t got = hero.readXYZ(s.rangeStart[i], s.rangeCount[i], xyz, radius);
        ASSERT_EQ(got, s.rangeCount[i]);
        double mx = 0, my = 0, mz = 0;
        for (size_t p = 0; p < got; ++p) { mx += xyz[p*3]; my += xyz[p*3+1]; mz += xyz[p*3+2]; }
        mx /= got; my /= got; mz /= got;
        // float32 readback of globe-scale coords → tolerance ~a metre.
        EXPECT_NEAR(mx, s.muX[i], 2.0);
        EXPECT_NEAR(my, s.muY[i], 2.0);
        EXPECT_NEAR(mz, s.muZ[i], 2.0);
    }
    std::remove(path.c_str());
}

// The fit must capture ribbon anisotropy: with one splat per ribbon, the
// covariance's largest diagonal should dominate the smallest (the long axis vs
// the thin/degenerate axes) — the whole reason splats beat axis-aligned quads.
TEST(StardsSummary, CapturesAnisotropy) {
    ControlNet net = buildRibbonNetwork(4, 500);
    GaussianSummary s = fit_gaussian_summary(net, 4);
    ASSERT_EQ(s.size(), 4u);
    for (size_t i = 0; i < s.size(); ++i) {
        double diag[3] = {s.s0[i], s.s3[i], s.s5[i]};
        double mx = std::max({diag[0], diag[1], diag[2]});
        double mn = std::min({diag[0], diag[1], diag[2]});
        EXPECT_GT(mx, 0.0);
        EXPECT_GT(mx, 100.0 * (mn + 1.0));  // strongly anisotropic
    }
}

// A cnet/3 file (summary layer added) must still read back as the full net,
// unchanged — the summary layer is additive and ignored by base-layer reads.
TEST(StardsSummary, FullReadParityWithSummary) {
    ControlNet net = buildRibbonNetwork(3, 300);
    std::string path = std::string(::testing::TempDir()) + "cnet3parity.stards";
    write_control_net_stards_summarized(net, path, 8);
    ControlNet back = read_control_net(path);   // ignores the "summary" layer
    ASSERT_EQ(back.numPoints(), net.numPoints());
    ASSERT_EQ(back.numMeasures(), net.numMeasures());
    for (size_t i = 0; i < net.numPoints(); ++i) {
        EXPECT_EQ(back.pointId[i], net.pointId[i]);
        EXPECT_DOUBLE_EQ(back.adjustedX[i], net.adjustedX[i]);
        EXPECT_DOUBLE_EQ(back.adjustedY[i], net.adjustedY[i]);
        EXPECT_DOUBLE_EQ(back.adjustedZ[i], net.adjustedZ[i]);
    }
    std::remove(path.c_str());
}

// Apriori-only nets (no adjusted coordinates) must still summarize, using the
// apriori XYZ fallback.
TEST(StardsSummary, AprioriFallback) {
    ControlNet net = buildRibbonNetwork(2, 400);
    // Flip: move adjusted coords to apriori, clear adjusted presence.
    net.aprioriX = net.adjustedX; net.aprioriY = net.adjustedY; net.aprioriZ = net.adjustedZ;
    net.hasApriori.assign(net.numPoints(), 1.0);
    net.hasAdjusted.assign(net.numPoints(), 0.0);
    GaussianSummary s = fit_gaussian_summary(net, 2);
    ASSERT_EQ(s.size(), 2u);
    uint64_t wsum = 0;
    for (size_t i = 0; i < s.size(); ++i) wsum += s.weight[i];
    EXPECT_EQ(wsum, net.numPoints());  // fitted from apriori, not adjusted
}

namespace {

// A whole point extracted as a self-contained record: its id/coords plus its
// measures (serial + sample/line) and its covariance element counts. Used to
// compare two nets as SETS of points regardless of point order.
struct PointRecord {
    std::string id;
    double ax, ay, az; uint8_t hasAdj;
    std::vector<std::string> measSN;
    std::vector<double> measSample;
    size_t aprioriCovarN, adjustedCovarN;
    bool operator<(const PointRecord& o) const { return id < o.id; }
};

PointRecord extractPoint(const ControlNet& n, size_t p) {
    PointRecord r;
    r.id = n.pointId[p];
    r.ax = n.adjustedX[p]; r.ay = n.adjustedY[p]; r.az = n.adjustedZ[p];
    r.hasAdj = static_cast<uint8_t>(n.hasAdjusted[p] != 0.0);
    uint32_t s = n.measureStart[p], c = n.measureCount[p];
    for (uint32_t m = 0; m < c; ++m) {
        r.measSN.push_back(n.serialNumber[s + m]);
        r.measSample.push_back(n.sample[s + m]);
    }
    r.aprioriCovarN = n.aprioriCovarOffset[p + 1] - n.aprioriCovarOffset[p];
    r.adjustedCovarN = n.adjustedCovarOffset[p + 1] - n.adjustedCovarOffset[p];
    return r;
}

std::vector<PointRecord> pointSet(const ControlNet& n) {
    std::vector<PointRecord> v;
    for (size_t p = 0; p < n.numPoints(); ++p) v.push_back(extractPoint(n, p));
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

// Reordering by track must be a permutation: the net is identical as a SET of
// whole points (each point keeps its measures + covariance), only order changes.
// This is the correctness gate for drill-down after a "tracks" summarize.
TEST(StardsSummary, ReorderIsPermutation) {
    ControlNet net = buildTestNetwork();  // p1 (2 meas + covar), p2 (1 meas), p3 (0)
    ControlNet reordered = reorder_points_by_track(net);

    ASSERT_EQ(reordered.numPoints(), net.numPoints());
    ASSERT_EQ(reordered.numMeasures(), net.numMeasures());

    std::vector<PointRecord> a = pointSet(net), b = pointSet(reordered);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].id, b[i].id);
        EXPECT_DOUBLE_EQ(a[i].ax, b[i].ax);
        EXPECT_DOUBLE_EQ(a[i].ay, b[i].ay);
        EXPECT_DOUBLE_EQ(a[i].az, b[i].az);
        EXPECT_EQ(a[i].hasAdj, b[i].hasAdj);
        EXPECT_EQ(a[i].measSN, b[i].measSN);         // measures moved WITH the point
        EXPECT_EQ(a[i].measSample, b[i].measSample);
        EXPECT_EQ(a[i].aprioriCovarN, b[i].aprioriCovarN);
        EXPECT_EQ(a[i].adjustedCovarN, b[i].adjustedCovarN);
    }
}

// Reordering must keep the CSR invariants intact: measureStart is the running
// prefix sum of measureCount, and covariance offsets are non-decreasing with the
// right totals. A broken permutation corrupts these.
TEST(StardsSummary, ReorderKeepsCsrValid) {
    ControlNet net = buildTestNetwork();
    ControlNet r = reorder_points_by_track(net);

    uint32_t running = 0;
    for (size_t p = 0; p < r.numPoints(); ++p) {
        EXPECT_EQ(r.measureStart[p], running);
        running += r.measureCount[p];
    }
    EXPECT_EQ(running, r.numMeasures());

    ASSERT_EQ(r.aprioriCovarOffset.size(), r.numPoints() + 1);
    ASSERT_EQ(r.adjustedCovarOffset.size(), r.numPoints() + 1);
    EXPECT_EQ(r.aprioriCovarOffset.front(), 0u);
    EXPECT_EQ(r.aprioriCovarOffset.back(), r.aprioriCovar.size());
    EXPECT_EQ(r.adjustedCovarOffset.back(), r.adjustedCovar.size());
    for (size_t p = 0; p < r.numPoints(); ++p) {
        EXPECT_LE(r.aprioriCovarOffset[p], r.aprioriCovarOffset[p + 1]);
        EXPECT_LE(r.adjustedCovarOffset[p], r.adjustedCovarOffset[p + 1]);
    }
}

// End-to-end "tracks" summarize: write a cnet/3 with byTracks=true, read it back
// as a full net (must equal the original as a set), and confirm each splat's
// [rangeStart,rangeCount) window drills down to points whose mean matches mu.
TEST(StardsSummary, TracksSummarizeDrillDown) {
    ControlNet net = buildRibbonNetwork(6, 300);  // 1800 points across 6 ribbons
    std::string path = std::string(::testing::TempDir()) + "tracks.stards";
    write_control_net_stards_summarized(net, path, 6, /*byTracks=*/true);

    // Full read-back equals the original as a SET (reorder is a permutation).
    ControlNet back = read_control_net(path);
    ASSERT_EQ(back.numPoints(), net.numPoints());
    std::vector<PointRecord> a = pointSet(net), b = pointSet(back);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].id, b[i].id);
        EXPECT_DOUBLE_EQ(a[i].ax, b[i].ax);
    }

    // Drill-down against the STORED (reordered) order.
    SummaryReader sr = SummaryReader::open(path);
    ASSERT_EQ(sr.count(), 6u);
    const GaussianSummary& s = sr.splats();
    HeroPointsReader hero = HeroPointsReader::open(path);
    for (size_t i = 0; i < s.size(); ++i) {
        std::vector<float> xyz; double radius = 0;
        size_t got = hero.readXYZ(s.rangeStart[i], s.rangeCount[i], xyz, radius);
        ASSERT_EQ(got, s.rangeCount[i]);
        double mx = 0, my = 0, mz = 0;
        for (size_t p = 0; p < got; ++p) { mx += xyz[p*3]; my += xyz[p*3+1]; mz += xyz[p*3+2]; }
        mx /= got; my /= got; mz /= got;
        EXPECT_NEAR(mx, s.muX[i], 2.0);
        EXPECT_NEAR(my, s.muY[i], 2.0);
        EXPECT_NEAR(mz, s.muZ[i], 2.0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Polyline ("lines") LOD summary (cnet/3)
// ---------------------------------------------------------------------------

// A cnet/3 "lines" net must: read back as the ORIGINAL net as a SET of points
// (reorder is a permutation), expose polylines whose vertices sit on the
// ellipsoid surface, and per-line point windows that drill down to real points.
TEST(StardsLines, WriteReadDrillDown) {
    ControlNet net = buildRibbonNetwork(6, 400);   // 6 elongated ribbons, adjusted coords
    std::string path = std::string(::testing::TempDir()) + "lines.stards";
    write_control_net_stards_lines(net, path);      // default options

    // 1. Full read-back equals the original as a SET (permutation invariant).
    ControlNet back = read_control_net(path);
    ASSERT_EQ(back.numPoints(), net.numPoints());
    std::vector<PointRecord> a = pointSet(net), b = pointSet(back);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].id, b[i].id);
        EXPECT_DOUBLE_EQ(a[i].ax, b[i].ax);
        EXPECT_DOUBLE_EQ(a[i].ay, b[i].ay);
        EXPECT_DOUBLE_EQ(a[i].az, b[i].az);
    }

    // 2. Lines exist and cover a good fraction of points (ribbons trace well).
    LinesReader lr = LinesReader::open(path);
    ASSERT_GT(lr.count(), 0u);
    const std::vector<float>& V = lr.verticesXYZ();
    ASSERT_GT(V.size(), 0u);

    // 3. Vertices lie on the Mars ellipsoid surface (radius ~3376–3396 km).
    for (size_t v = 0; v < lr.vertexCount(); ++v) {
        double x = V[v*3], y = V[v*3+1], z = V[v*3+2];
        double rad = std::sqrt(x*x + y*y + z*z);
        EXPECT_GT(rad, 3.0e6);
        EXPECT_LT(rad, 3.5e6);
    }

    // 4. Point windows partition-cover and each line's points sit near its
    //    polyline (perp distance bounded by the simplify tolerance + relief).
    HeroPointsReader hero = HeroPointsReader::open(path);
    uint64_t onLines = 0;
    for (size_t i = 0; i < lr.count(); ++i) {
        uint32_t rs, rc; lr.linePointRange(i, rs, rc);
        onLines += rc;
        EXPECT_LE(rs + rc, hero.count());
    }
    EXPECT_GT(onLines, 0u);
    EXPECT_LE(onLines, (uint64_t)net.numPoints());

    // 5. Drill-down: a line's window returns exactly that many real points.
    if (lr.count() > 0) {
        uint32_t rs, rc; lr.linePointRange(0, rs, rc);
        std::vector<float> xyz; double radius = 0;
        size_t got = hero.readXYZ(rs, rc, xyz, radius);
        EXPECT_EQ(got, rc);
    }
    std::remove(path.c_str());
}

// Lazy StarDS reader: open() reads shape only; readPoints() slices numeric
// columns via get_slice, windows string columns, and re-bases CSR offsets. A
// slice must equal the same points from an eager read.
TEST(StardsReader, LazyShapeAndSlices) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "lazy.stards";
    write_control_net(net, path);

    StardsControlNetReader r = StardsControlNetReader::open(path);
    ASSERT_EQ(r.numPoints(), net.numPoints());        // 3 points
    EXPECT_EQ(r.numMeasures(), net.numMeasures());     // 3 measures
    EXPECT_EQ(r.header().networkId, net.header.networkId);
    EXPECT_EQ(r.pointId(0), "p1");
    EXPECT_EQ(r.pointId(2), "p3");

    uint32_t first, count;
    r.measureRange(0, first, count);
    EXPECT_EQ(first, 0u);
    EXPECT_EQ(count, 2u);           // p1 owns 2 measures
    r.measureRange(2, first, count);
    EXPECT_EQ(count, 0u);           // p3 owns 0 measures

    // readPoints(0,1) -> just p1, with both measures and its covariance/log.
    ControlNet slice = r.readPoints(0, 1);
    ASSERT_EQ(slice.numPoints(), 1u);
    EXPECT_EQ(slice.pointId[0], "p1");
    EXPECT_EQ(slice.measureCount[0], 2u);
    EXPECT_EQ(slice.measureStart[0], 0u);
    ASSERT_EQ(slice.numMeasures(), 2u);
    EXPECT_TRUE(slice.hasApriori[0]);
    EXPECT_DOUBLE_EQ(slice.aprioriX[0], net.aprioriX[0]);
    // Covariance re-based to the slice.
    ASSERT_EQ(slice.aprioriCovarOffset.size(), 2u);
    EXPECT_EQ(slice.aprioriCovarOffset[0], 0u);
    EXPECT_EQ(slice.aprioriCovarOffset[1], net.aprioriCovar.size());
    ASSERT_EQ(slice.aprioriCovar.size(), net.aprioriCovar.size());
    for (size_t i = 0; i < net.aprioriCovar.size(); ++i)
        EXPECT_DOUBLE_EQ(slice.aprioriCovar[i], net.aprioriCovar[i]);
    // Measure log for the first measure re-based to the slice.
    EXPECT_EQ(slice.measureLogOffset.front(), 0u);
    ASSERT_EQ(slice.serialNumber[0], "SN_A");
    ASSERT_EQ(slice.serialNumber[1], "SN_B");

    std::remove(path.c_str());
}

// A lazily-read tail slice (p2, p3) equals those points from an eager read.
TEST(StardsReader, TailSliceMatchesEager) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "tail.stards";
    write_control_net(net, path);

    ControlNet full = read_control_net(path);
    StardsControlNetReader r = StardsControlNetReader::open(path);
    ControlNet tail = r.readPoints(1, 2);

    ASSERT_EQ(tail.numPoints(), 2u);
    EXPECT_EQ(tail.pointId[0], "p2");
    EXPECT_EQ(tail.pointId[1], "p3");
    EXPECT_EQ(tail.measureCount[0], 1u);
    EXPECT_EQ(tail.measureCount[1], 0u);      // p3 zero-measure
    EXPECT_EQ(tail.measureStart[0], 0u);      // re-based
    ASSERT_EQ(tail.numMeasures(), 1u);
    EXPECT_EQ(tail.serialNumber[0], full.serialNumber[full.measureStart[1]]);
    EXPECT_EQ(tail.pointType[0], full.pointType[1]);
    EXPECT_EQ(tail.pointType[1], full.pointType[2]);

    std::remove(path.c_str());
}

// readAll() via the lazy reader equals the eager reader field-for-field.
TEST(StardsReader, ReadAllEqualsEager) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "rall.stards";
    write_control_net(net, path);
    ControlNet eager = read_control_net(path);
    ControlNet viaReader = StardsControlNetReader::open(path).readAll();
    expectEqual(eager, viaReader);
    std::remove(path.c_str());
}

// readMeasures() snaps a raw measure window to the owning whole points.
TEST(StardsReader, MeasureWindowSnapsToPoints) {
    ControlNet net = buildTestNetwork();
    std::string path = std::string(::testing::TempDir()) + "mw.stards";
    write_control_net(net, path);

    StardsControlNetReader r = StardsControlNetReader::open(path);
    // Row 1 belongs to p1 (which owns measures 0..1); the window snaps to p1.
    ControlNet w = r.readMeasures(1, 1);
    ASSERT_GE(w.numPoints(), 1u);
    EXPECT_EQ(w.pointId[0], "p1");
    EXPECT_EQ(w.measureCount[0], 2u);

    std::remove(path.c_str());
}

// Normalized cnet/2 format: a net with many measures sharing few serial numbers
// round-trips exactly through dictionary encoding (dict + int32 index). Also
// confirms the GZIP_SHUFFLE_BLOCK numeric columns remain sliceable via the lazy
// reader (a slice must equal the eager read).
TEST(Cnet, StardsNormalizedDictRoundTrip) {
    ControlNet net = buildTestNetwork();
    // Add many measures to p1, all referencing one of just 3 serials, so the
    // serial column is highly repetitive (the case dictionary encoding targets).
    const char* serials[3] = {"IMG_A", "IMG_B", "IMG_C"};
    for (int k = 0; k < 300; ++k) {
        net.serialNumber.push_back(serials[k % 3]);
        net.measureType.push_back(MeasureType::RegisteredPixel);
        net.sample.push_back(k * 1.5); net.hasSample.push_back(1);
        net.line.push_back(k * 2.5); net.hasLine.push_back(1);
        net.sampleResidual.push_back(0); net.hasSampleResidual.push_back(0);
        net.lineResidual.push_back(0); net.hasLineResidual.push_back(0);
        net.measureChooserName.push_back("reg"); net.measureDatetime.push_back("2024-01-01T00:00:00");
        net.measureEditLock.push_back(0); net.hasMeasureEditLock.push_back(0);
        net.measureIgnore.push_back(0); net.hasMeasureIgnore.push_back(0);
        net.measureJigsawRejected.push_back(0); net.hasMeasureJigsawRejected.push_back(0);
        net.diameter.push_back(0); net.hasDiameter.push_back(0);
        net.aprioriSample.push_back(0); net.hasAprioriSample.push_back(0);
        net.aprioriLine.push_back(0); net.hasAprioriLine.push_back(0);
        net.sampleSigma.push_back(0.5); net.hasSampleSigma.push_back(1);
        net.lineSigma.push_back(0.5); net.hasLineSigma.push_back(1);
        net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
    }
    net.measureCount[0] += 300;  // p1 owns the new measures

    std::string path = std::string(::testing::TempDir()) + "norm.stards";
    write_control_net(net, path);
    ControlNet back = read_control_net(path);
    expectEqual(net, back);
    // Exact serial-string reconstruction from the dictionary.
    ASSERT_EQ(back.serialNumber.size(), net.serialNumber.size());
    for (size_t m = 0; m < net.serialNumber.size(); ++m)
        EXPECT_EQ(back.serialNumber[m], net.serialNumber[m]);

    // Sliceability under GZIP_SHUFFLE_BLOCK: the lazy reader's numeric slice for
    // p1 must equal the eager values.
    StardsControlNetReader r = StardsControlNetReader::open(path);
    ControlNet p1 = r.readPoints(0, 1);
    ASSERT_EQ(p1.numMeasures(), net.measureCount[0]);
    for (uint32_t m = 0; m < p1.numMeasures(); ++m) {
        EXPECT_DOUBLE_EQ(p1.sample[m], net.sample[net.measureStart[0] + m]);
        EXPECT_EQ(p1.serialNumber[m], net.serialNumber[net.measureStart[0] + m]);
    }
    std::remove(path.c_str());
}

// ===========================================================================
// miniparquet — the header-only Parquet reader (external/miniparquet.hpp).
// ===========================================================================

namespace {
// A RangeReader that counts total bytes served — proves ranged reads don't slurp
// the whole file for a small slice.
class CountingReader : public mparq::RangeReader {
  public:
    explicit CountingReader(std::vector<uint8_t> b) : bytes_(std::move(b)) {}
    size_t read_at(size_t off, size_t len, std::vector<uint8_t>& out) override {
        if (off >= bytes_.size()) { out.clear(); return 0; }
        size_t end = std::min(off + len, bytes_.size());
        out.assign(bytes_.begin() + off, bytes_.begin() + end);
        served += out.size();
        return out.size();
    }
    size_t size_or_unknown() override { return bytes_.size(); }
    bool good() const override { return true; }
    size_t served = 0;
  private:
    std::vector<uint8_t> bytes_;
};
}  // namespace

// The header-only writer round-trips through the header-only reader (bytes).
TEST(MiniParquet, WriterReaderBytesRoundTrip) {
    ControlNet net = buildTestNetwork();
    std::vector<uint8_t> bytes = write_control_net_miniparquet_bytes(net);
    ControlNet back = read_control_net_miniparquet_bytes(bytes);
    expectEqual(net, back);
}

// GZIP-compressed Parquet round-trips identically. Guards the gzip page path
// (incl. the tiny-page wrapper-overhead bound that once made deflate fail).
TEST(MiniParquet, GzipCompressedRoundTrips) {
    ControlNet net = buildTestNetwork();
    std::vector<uint8_t> gz = write_control_net_miniparquet_bytes(net, ParquetCompression::Gzip);
    ControlNet back = read_control_net_miniparquet_bytes(gz);
    expectEqual(net, back);
    // (No size assertion here: a 3-point net is dominated by per-page gzip
    //  wrapper overhead. The size win is exercised by the large-net test below.)
}

// The default dispatch path (write + read via read_control_net/write_control_net
// on a .parquet) round-trips — both now go through miniparquet.
TEST(MiniParquet, DefaultPathRoundTrip) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/mp2.parquet");
    ControlNet back = read_control_net("/vsimem/mp2.parquet");
    expectEqual(net, back);
    VSIUnlink("/vsimem/mp2.parquet");
}

// miniparquet reads a real parquet-cpp-arrow file (Snappy, RLE_DICTIONARY),
// proving cross-tool read compatibility with genuine Arrow output. The fixture
// is committed at tests/data/cnet/arrow_golden.parquet (see MINISET_TEST_DATA).
TEST(MiniParquet, ReadsArrowGoldenFixture) {
    std::string path = std::string(MINISET_TEST_DATA) + "/cnet/arrow_golden.parquet";
    ControlNet net = read_control_net(path);
    EXPECT_EQ(net.numPoints(), 1893u);
    EXPECT_EQ(net.numMeasures(), 3786u);
    EXPECT_EQ(net.header.networkId, "test");
    // First point/measure sanity (values verified against the Arrow reader).
    EXPECT_EQ(net.pointId[0], "cloud00001");
    EXPECT_EQ(net.pointType[0], PointType::Free);
    ASSERT_GT(net.numMeasures(), 0u);
    EXPECT_TRUE(net.hasSample[0]);
    EXPECT_NEAR(net.sample[0], 2471.834, 1e-2);
}

// Column projection reads only the requested column's chunk(s), not the whole
// file. Uses a byte-counting RangeReader over a synthesized many-row file so the
// projected `id` read is provably a fraction of the total bytes.
TEST(MiniParquet, ProjectionReadsFractionOfFile) {
    // Build a net with many measures so column chunks dominate the footer.
    ControlNet net = buildTestNetwork();
    // Duplicate p1's measures many times to inflate the file well past the
    // footer-tail window, giving column chunks meaningful size.
    for (int rep = 0; rep < 400; ++rep) {
        net.serialNumber.push_back("SN_" + std::to_string(rep));
        net.measureType.push_back(MeasureType::RegisteredPixel);
        net.sample.push_back(rep + 0.5); net.hasSample.push_back(1);
        net.line.push_back(rep + 1.5); net.hasLine.push_back(1);
        net.sampleResidual.push_back(0); net.hasSampleResidual.push_back(0);
        net.lineResidual.push_back(0); net.hasLineResidual.push_back(0);
        net.measureChooserName.push_back(""); net.measureDatetime.push_back("");
        net.measureEditLock.push_back(0); net.hasMeasureEditLock.push_back(0);
        net.measureIgnore.push_back(0); net.hasMeasureIgnore.push_back(0);
        net.measureJigsawRejected.push_back(0); net.hasMeasureJigsawRejected.push_back(0);
        net.diameter.push_back(0); net.hasDiameter.push_back(0);
        net.aprioriSample.push_back(0); net.hasAprioriSample.push_back(0);
        net.aprioriLine.push_back(0); net.hasAprioriLine.push_back(0);
        net.sampleSigma.push_back(0); net.hasSampleSigma.push_back(0);
        net.lineSigma.push_back(0); net.hasLineSigma.push_back(0);
        net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
        net.measureCount[0] += 1;
    }
    std::vector<uint8_t> bytes = write_control_net_miniparquet_bytes(net);
    size_t total = bytes.size();

    auto reader = std::make_unique<CountingReader>(bytes);
    CountingReader* cr = reader.get();
    mparq::Reader r = mparq::Reader::fromReader(std::move(reader));
    EXPECT_EQ(r.numRows(), net.numMeasures() + 1);  // + zero-measure sentinel row

    // Read just the `id` column; served bytes must be a fraction of the file
    // (the wide double columns — sample/line/etc. — are never fetched).
    size_t before = cr->served;
    auto cols = r.readColumns({"id"});
    size_t idBytes = cr->served - before;
    EXPECT_EQ(cols["id"].str.size(), net.numMeasures() + 1);
    EXPECT_LT(idBytes, total / 2);  // projection paid off
    VSIUnlink("/vsimem/mp3.parquet");
}

// A projected slice through the lazy reader equals the eager full read.
TEST(MiniParquet, LazyReaderProjectionMatchesEager) {
    ControlNet net = buildTestNetwork();
    write_control_net(net, "/vsimem/mp4.parquet");

    ControlNetReader r = ControlNetReader::open("/vsimem/mp4.parquet");
    EXPECT_TRUE(r.supportsLazy());
    ASSERT_EQ(r.numPoints(), net.numPoints());

    ReadOptions opts;
    opts.columns = {"id", "type", "serialnumber", "measure_type", "sample", "line"};
    ControlNet slice = r.readPoints(0, 1, opts);
    ASSERT_EQ(slice.numPoints(), 1u);
    EXPECT_EQ(slice.pointId[0], "p1");
    ASSERT_EQ(slice.numMeasures(), 2u);
    EXPECT_TRUE(slice.hasSample[0]);
    // Covariance columns were not requested → absent.
    EXPECT_EQ(slice.aprioriCovar.size(), 0u);
    VSIUnlink("/vsimem/mp4.parquet");
}

// Unsupported input fails loudly rather than returning silently-wrong data.
TEST(MiniParquet, RejectsNonParquetBytes) {
    std::vector<uint8_t> junk = {'N', 'O', 'T', 'P', 'A', 'R', 'Q', 'U', 'E', 'T'};
    EXPECT_THROW(read_control_net_miniparquet_bytes(junk), std::exception);
}

// Simulate the remote/lazy path (as WASM's EmscriptenFetchRangeReader does): a
// CountingReader stands in for ranged HTTP GETs. Opening reads shape from the
// footer + id column, and a single-point slice fetches only its row window — the
// whole "file" is never downloaded. This is the cloud-optimized behavior the
// emscripten_fetch backend provides in the browser.
TEST(MiniParquet, LazyRangedReadDoesNotDownloadWholeFile) {
    // A wide file (many rows) written by our own writer.
    ControlNet net = buildTestNetwork();
    for (int rep = 0; rep < 800; ++rep) {
        net.serialNumber.push_back("SN_" + std::to_string(rep));
        net.measureType.push_back(MeasureType::RegisteredPixel);
        net.sample.push_back(rep + 0.25); net.hasSample.push_back(1);
        net.line.push_back(rep + 0.75); net.hasLine.push_back(1);
        net.sampleResidual.push_back(0); net.hasSampleResidual.push_back(0);
        net.lineResidual.push_back(0); net.hasLineResidual.push_back(0);
        net.measureChooserName.push_back(""); net.measureDatetime.push_back("");
        net.measureEditLock.push_back(0); net.hasMeasureEditLock.push_back(0);
        net.measureIgnore.push_back(0); net.hasMeasureIgnore.push_back(0);
        net.measureJigsawRejected.push_back(0); net.hasMeasureJigsawRejected.push_back(0);
        net.diameter.push_back(0); net.hasDiameter.push_back(0);
        net.aprioriSample.push_back(0); net.hasAprioriSample.push_back(0);
        net.aprioriLine.push_back(0); net.hasAprioriLine.push_back(0);
        net.sampleSigma.push_back(0); net.hasSampleSigma.push_back(0);
        net.lineSigma.push_back(0); net.hasLineSigma.push_back(0);
        net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
        net.measureCount[0] += 1;
    }
    std::vector<uint8_t> bytes = write_control_net_miniparquet_bytes(net);
    size_t total = bytes.size();

    // Open reads only footer + id column (not the whole file).
    auto reader = std::make_unique<CountingReader>(bytes);
    CountingReader* cr = reader.get();
    mparq::Reader r = mparq::Reader::fromReader(std::move(reader));
    EXPECT_EQ(r.numRows(), net.numMeasures() + 1);
    size_t afterOpen = cr->served;

    // Project just id+sample for the first point (2 original rows). The heavy
    // remaining columns are never fetched.
    size_t before = cr->served;
    auto cols = r.readColumns({"id", "sample"}, 0, 1);
    size_t sliceBytes = cr->served - before;
    EXPECT_GT(cols["id"].str.size(), 0u);
    EXPECT_LT(afterOpen + sliceBytes, total);  // never downloaded the whole file
}

// ===========================================================================
// Streaming / large-file / range-query behavior.
// ===========================================================================

namespace {
// Build a net with `nPoints` single-measure points; measure `sample` = point
// index (so a range query on `sample` selects a contiguous point range). Each
// point also carries a 6-value apriori covariance to exercise list columns.
ControlNet buildWideNet(int nPoints) {
    ControlNet net;
    net.header.networkId = "Wide";
    net.header.targetName = "Mars";
    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);
    for (int p = 0; p < nPoints; ++p) {
        net.pointId.push_back("pt" + std::to_string(p));
        net.pointType.push_back(PointType::Free);
        net.chooserName.push_back(""); net.datetime.push_back("");
        net.editLock.push_back(0); net.hasEditLock.push_back(0);
        net.ignore.push_back(0); net.hasIgnore.push_back(0);
        net.jigsawRejected.push_back(0); net.hasJigsawRejected.push_back(0);
        net.referenceIndex.push_back(0); net.hasReferenceIndex.push_back(0);
        net.aprioriSurfPointSource.push_back(AprioriSource::None);
        net.aprioriSurfPointSourceFile.push_back("");
        net.aprioriRadiusSource.push_back(AprioriSource::None);
        net.aprioriRadiusSourceFile.push_back("");
        net.aprioriX.push_back(0); net.aprioriY.push_back(0); net.aprioriZ.push_back(0); net.hasApriori.push_back(0);
        net.adjustedX.push_back(0); net.adjustedY.push_back(0); net.adjustedZ.push_back(0); net.hasAdjusted.push_back(0);
        for (double v : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}) net.aprioriCovar.push_back(v);
        net.aprioriCovarOffset.push_back(static_cast<uint32_t>(net.aprioriCovar.size()));
        net.adjustedCovarOffset.push_back(static_cast<uint32_t>(net.adjustedCovar.size()));
        net.measureStart.push_back(static_cast<uint32_t>(net.numMeasures()));
        net.measureCount.push_back(1);
        // one measure
        net.serialNumber.push_back("SN" + std::to_string(p));
        net.measureType.push_back(MeasureType::RegisteredPixel);
        net.sample.push_back(static_cast<double>(p)); net.hasSample.push_back(1);
        net.line.push_back(static_cast<double>(p) * 2.0); net.hasLine.push_back(1);
        net.sampleResidual.push_back(0); net.hasSampleResidual.push_back(0);
        net.lineResidual.push_back(0); net.hasLineResidual.push_back(0);
        net.measureChooserName.push_back(""); net.measureDatetime.push_back("");
        net.measureEditLock.push_back(0); net.hasMeasureEditLock.push_back(0);
        net.measureIgnore.push_back(0); net.hasMeasureIgnore.push_back(0);
        net.measureJigsawRejected.push_back(0); net.hasMeasureJigsawRejected.push_back(0);
        net.diameter.push_back(0); net.hasDiameter.push_back(0);
        net.aprioriSample.push_back(0); net.hasAprioriSample.push_back(0);
        net.aprioriLine.push_back(0); net.hasAprioriLine.push_back(0);
        net.sampleSigma.push_back(0); net.hasSampleSigma.push_back(0);
        net.lineSigma.push_back(0); net.hasLineSigma.push_back(0);
        net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
    }
    return net;
}
}  // namespace

// The streaming writer emits multiple bounded row groups; the reader sees them
// all and round-trips the data. (Small row-group target via the env knob.)
TEST(MiniParquetLarge, MultiRowGroupWriteReadRoundTrip) {
    ControlNet net = buildWideNet(2500);
    setenv("MINISET_CNET_ROWGROUP_ROWS", "500", 1);
    std::vector<uint8_t> bytes = write_control_net_miniparquet_bytes(net);
    unsetenv("MINISET_CNET_ROWGROUP_ROWS");

    mparq::Reader r = mparq::Reader::openBytes(bytes);
    EXPECT_GT(r.numRowGroups(), 1u);           // genuinely chunked
    EXPECT_EQ(r.numRows(), net.numMeasures());  // one row per measure (no zero-measure pts)

    ControlNet back = read_control_net_miniparquet_bytes(bytes);
    ASSERT_EQ(back.numPoints(), net.numPoints());
    ASSERT_EQ(back.numMeasures(), net.numMeasures());
    EXPECT_EQ(back.pointId[0], "pt0");
    EXPECT_EQ(back.pointId[2499], "pt2499");
    EXPECT_DOUBLE_EQ(back.sample[1234], 1234.0);
    // Covariances survived the row-group chunking.
    EXPECT_EQ(back.aprioriCovar.size(), net.aprioriCovar.size());
}

// At scale, GZIP-compressed Parquet is materially smaller than uncompressed and
// still round-trips. (2500 points -> per-page gzip overhead is amortized.)
TEST(MiniParquetLarge, GzipSmallerAtScale) {
    ControlNet net = buildWideNet(2500);
    std::vector<uint8_t> raw = write_control_net_miniparquet_bytes(net, ParquetCompression::None);
    std::vector<uint8_t> gz = write_control_net_miniparquet_bytes(net, ParquetCompression::Gzip);
    ControlNet back = read_control_net_miniparquet_bytes(gz);
    ASSERT_EQ(back.numPoints(), net.numPoints());
    ASSERT_EQ(back.numMeasures(), net.numMeasures());
    EXPECT_LT(gz.size(), raw.size());
}

// GDAL/Arrow reads a multi-row-group file the streaming writer produced.
TEST(MiniParquetLarge, GdalReadsMultiRowGroupOutput) {
    ControlNet net = buildWideNet(2000);
    setenv("MINISET_CNET_ROWGROUP_ROWS", "300", 1);
    write_control_net(net, "/vsimem/big.parquet");
    unsetenv("MINISET_CNET_ROWGROUP_ROWS");

    GDALAllRegister();
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx("/vsimem/big.parquet", GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(true), static_cast<GIntBig>(net.numMeasures()));
    GDALClose(ds);
    VSIUnlink("/vsimem/big.parquet");
}

// Range query prunes row groups by statistics and returns exactly the matching
// measures. `sample` = point index, so [1000,1099] selects exactly 100 points.
TEST(MiniParquetLarge, RangeQueryPrunesAndMatches) {
    ControlNet net = buildWideNet(3000);  // sample = 0..2999
    setenv("MINISET_CNET_ROWGROUP_ROWS", "300", 1);  // ~10 row groups
    write_control_net(net, "/vsimem/rq.parquet");
    unsetenv("MINISET_CNET_ROWGROUP_ROWS");

    ControlNet q = read_where_miniparquet("/vsimem/rq.parquet", "sample", 1000, 1099);
    ASSERT_EQ(q.numMeasures(), 100u);
    for (size_t m = 0; m < q.numMeasures(); ++m) {
        EXPECT_TRUE(q.hasSample[m]);
        EXPECT_GE(q.sample[m], 1000.0);
        EXPECT_LE(q.sample[m], 1099.0);
    }
    // Result points carry their covariance (present on the matching row).
    EXPECT_EQ(q.numPoints(), 100u);
    VSIUnlink("/vsimem/rq.parquet");
}

// A range that spans no row group's stats returns nothing (full prune).
TEST(MiniParquetLarge, RangeQueryEmptyWhenDisjoint) {
    ControlNet net = buildWideNet(500);  // sample 0..499
    write_control_net(net, "/vsimem/rq2.parquet");
    ControlNet q = read_where_miniparquet("/vsimem/rq2.parquet", "sample", 100000, 200000);
    EXPECT_EQ(q.numMeasures(), 0u);
    VSIUnlink("/vsimem/rq2.parquet");
}

// The incremental ControlNetParquetWriter (bounded-memory streaming) produces a
// file whose content equals a one-shot write of the same net.
TEST(MiniParquetLarge, IncrementalWriterRoundTrips) {
    ControlNet net = buildWideNet(1000);
    // Source file to read whole-point batches from.
    write_control_net(net, "/vsimem/incr_src.parquet");
    ControlNetReader src = ControlNetReader::open("/vsimem/incr_src.parquet");

    std::string path = std::string(::testing::TempDir()) + "incr.parquet";
    {
        ControlNetParquetWriter w(path);
        size_t nPts = src.numPoints();
        for (size_t start = 0; start < nPts; start += 250) {
            size_t count = std::min<size_t>(250, nPts - start);
            w.writeBatch(src.readPoints(start, count));
        }
        w.finish();
    }
    VSIUnlink("/vsimem/incr_src.parquet");

    ControlNet back = read_control_net(path);
    expectEqual(net, back);
    std::remove(path.c_str());
}
