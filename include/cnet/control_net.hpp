#ifndef MINISET_CNET_CONTROL_NET_HPP
#define MINISET_CNET_CONTROL_NET_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cnet {

/// Point type. Values match ISIS ControlPointFileEntryV0002.PointType so enum
/// integers round-trip through both the protobuf .net and Parquet formats.
enum class PointType : int32_t {
    ObsoleteTie = 0,
    ObsoleteGround = 1,
    Free = 2,
    Constrained = 3,
    Fixed = 4,
};

/// Apriori surface-point / radius source. Matches ISIS AprioriSource.
enum class AprioriSource : int32_t {
    None = 0,
    User = 1,
    AverageOfMeasures = 2,
    Reference = 3,
    Ellipsoid = 4,
    DEM = 5,
    Basemap = 6,
    BundleSolution = 7,
};

/// Measure type. Matches ISIS ControlPointFileEntryV0002.Measure.MeasureType.
enum class MeasureType : int32_t {
    Candidate = 0,
    Manual = 1,
    RegisteredPixel = 2,
    RegisteredSubPixel = 3,
};

/// Network-level header (repeated on every Parquet row; a single protobuf
/// header message in the .net format).
struct NetworkHeader {
    std::string networkId;
    std::string targetName;
    std::string created;
    std::string lastModified;
    std::string description;
    std::string userName;
};

/// Data-oriented (struct-of-arrays) control network.
///
/// Points and measures are stored as parallel column vectors rather than as
/// per-object allocations, so bulk file I/O maps to large contiguous copies and
/// processing is cache-friendly. A point at index p owns the measures in the
/// half-open range [measureStart[p], measureStart[p] + measureCount[p]).
///
/// Optional scalar fields carry a parallel `has*` bitset (0/1 per element) that
/// mirrors protobuf field presence, so absent fields survive a round-trip
/// distinct from a present-but-zero value. Variable-length fields (covariances,
/// measure logs) use CSR-style flattened value + offset arrays.
struct ControlNet {
    NetworkHeader header;

    // ---- Point columns (size == numPoints) --------------------------------
    std::vector<std::string> pointId;
    std::vector<PointType> pointType;
    std::vector<std::string> chooserName;
    std::vector<std::string> datetime;

    std::vector<uint8_t> editLock,        hasEditLock;
    std::vector<uint8_t> ignore,          hasIgnore;
    std::vector<uint8_t> jigsawRejected,  hasJigsawRejected;
    std::vector<int32_t> referenceIndex,  hasReferenceIndex;

    std::vector<AprioriSource> aprioriSurfPointSource;  // always present
    std::vector<std::string>   aprioriSurfPointSourceFile;
    std::vector<AprioriSource> aprioriRadiusSource;     // always present
    std::vector<std::string>   aprioriRadiusSourceFile;

    std::vector<double> aprioriX, aprioriY, aprioriZ, hasApriori;   // XYZ share one flag
    std::vector<double> adjustedX, adjustedY, adjustedZ, hasAdjusted;

    // CSR-packed covariances (<=6 upper-triangular doubles per point).
    std::vector<double> aprioriCovar;
    std::vector<uint32_t> aprioriCovarOffset;   // size numPoints + 1
    std::vector<double> adjustedCovar;
    std::vector<uint32_t> adjustedCovarOffset;  // size numPoints + 1

    // Measure ownership: measures [measureStart[p], measureStart[p]+measureCount[p]).
    std::vector<uint32_t> measureStart;   // size numPoints
    std::vector<uint32_t> measureCount;   // size numPoints

    // ---- Measure columns (size == numMeasures) ----------------------------
    std::vector<std::string> serialNumber;
    std::vector<MeasureType> measureType;
    std::vector<double> sample,          hasSample;
    std::vector<double> line,            hasLine;
    std::vector<double> sampleResidual,  hasSampleResidual;
    std::vector<double> lineResidual,    hasLineResidual;
    std::vector<std::string> measureChooserName;
    std::vector<std::string> measureDatetime;
    std::vector<uint8_t> measureEditLock,       hasMeasureEditLock;
    std::vector<uint8_t> measureIgnore,         hasMeasureIgnore;
    std::vector<uint8_t> measureJigsawRejected, hasMeasureJigsawRejected;
    std::vector<double> diameter,       hasDiameter;
    std::vector<double> aprioriSample,  hasAprioriSample;
    std::vector<double> aprioriLine,    hasAprioriLine;
    std::vector<double> sampleSigma,    hasSampleSigma;
    std::vector<double> lineSigma,      hasLineSigma;

    // CSR-packed per-measure double log data (type + value parallel arrays).
    std::vector<int32_t> measureLogType;
    std::vector<double>  measureLogValue;
    std::vector<uint32_t> measureLogOffset;  // size numMeasures + 1

    size_t numPoints() const { return pointId.size(); }
    size_t numMeasures() const { return serialNumber.size(); }
};

}  // namespace cnet

#endif  // MINISET_CNET_CONTROL_NET_HPP
