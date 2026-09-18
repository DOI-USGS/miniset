#include "cnet/column_source.hpp"

#include <cstddef>

namespace cnet {

namespace {
// Safe element access for optional/short columns (returns default if absent).
template <typename V>
typename V::value_type at(const V& v, size_t i, typename V::value_type dflt = {}) {
    return i < v.size() ? v[i] : dflt;
}
}  // namespace

ControlNet control_net_from_columns(const ColumnBatch& b) {
    ControlNet net;
    net.header.networkId = b.networkId;
    net.header.targetName = b.targetName;
    net.header.created = b.created;
    net.header.lastModified = b.lastModified;
    net.header.description = b.description;
    net.header.userName = b.userName;

    net.aprioriCovarOffset.push_back(0);
    net.adjustedCovarOffset.push_back(0);
    net.measureLogOffset.push_back(0);

    const size_t rows = b.numRows();
    size_t covA = 0, covD = 0, logPos = 0;  // running offsets into flattened lists
    bool havePoint = false;
    std::string currentId;

    for (size_t r = 0; r < rows; ++r) {
        const std::string& rowId = b.id[r];
        if (!havePoint || rowId != currentId) {
            havePoint = true;
            currentId = rowId;
            net.pointId.push_back(rowId);
            net.pointType.push_back(static_cast<PointType>(at(b.type, r)));
            net.chooserName.push_back(at(b.chooserName, r, std::string()));
            net.datetime.push_back(at(b.datetime, r, std::string()));
            net.editLock.push_back(at(b.editLock, r) != 0); net.hasEditLock.push_back(at(b.hasEditLock, r) != 0);
            net.ignore.push_back(at(b.ignore, r) != 0); net.hasIgnore.push_back(at(b.hasIgnore, r) != 0);
            net.jigsawRejected.push_back(at(b.jigsawRejected, r) != 0); net.hasJigsawRejected.push_back(at(b.hasJigsawRejected, r) != 0);
            net.referenceIndex.push_back(at(b.referenceIndex, r)); net.hasReferenceIndex.push_back(at(b.hasReferenceIndex, r) != 0);
            net.aprioriSurfPointSource.push_back(static_cast<AprioriSource>(at(b.aprioriSurfPointSource, r)));
            net.aprioriSurfPointSourceFile.push_back(at(b.aprioriSurfPointSourceFile, r, std::string()));
            net.aprioriRadiusSource.push_back(static_cast<AprioriSource>(at(b.aprioriRadiusSource, r)));
            net.aprioriRadiusSourceFile.push_back(at(b.aprioriRadiusSourceFile, r, std::string()));
            net.aprioriX.push_back(at(b.aprioriX, r)); net.aprioriY.push_back(at(b.aprioriY, r)); net.aprioriZ.push_back(at(b.aprioriZ, r));
            net.hasApriori.push_back(at(b.hasApriori, r));
            net.adjustedX.push_back(at(b.adjustedX, r)); net.adjustedY.push_back(at(b.adjustedY, r)); net.adjustedZ.push_back(at(b.adjustedZ, r));
            net.hasAdjusted.push_back(at(b.hasAdjusted, r));

            int32_t na = at(b.aprioriCovarLen, r);
            for (int32_t k = 0; k < na && covA < b.aprioriCovar.size(); ++k) net.aprioriCovar.push_back(b.aprioriCovar[covA++]);
            net.aprioriCovarOffset.push_back(static_cast<uint32_t>(net.aprioriCovar.size()));
            int32_t nd = at(b.adjustedCovarLen, r);
            for (int32_t k = 0; k < nd && covD < b.adjustedCovar.size(); ++k) net.adjustedCovar.push_back(b.adjustedCovar[covD++]);
            net.adjustedCovarOffset.push_back(static_cast<uint32_t>(net.adjustedCovar.size()));

            net.measureStart.push_back(static_cast<uint32_t>(net.numMeasures()));
            net.measureCount.push_back(0);
        } else {
            // Covariance list positions still advance for skipped point-duplicate rows.
            covA += at(b.aprioriCovarLen, r);
            covD += at(b.adjustedCovarLen, r);
        }

        // Measure (rows with an empty serial number are the zero-measure sentinel).
        const std::string& serial = at(b.serialnumber, r, std::string());
        int32_t logLen = at(b.measureLogLen, r);
        if (!serial.empty()) {
            net.serialNumber.push_back(serial);
            net.measureType.push_back(static_cast<MeasureType>(at(b.measureType, r)));
            net.sample.push_back(at(b.sample, r)); net.hasSample.push_back(at(b.hasSample, r));
            net.line.push_back(at(b.line, r)); net.hasLine.push_back(at(b.hasLine, r));
            net.sampleResidual.push_back(at(b.sampleResidual, r)); net.hasSampleResidual.push_back(at(b.hasSampleResidual, r));
            net.lineResidual.push_back(at(b.lineResidual, r)); net.hasLineResidual.push_back(at(b.hasLineResidual, r));
            net.measureChooserName.push_back(at(b.measureChooserName, r, std::string()));
            net.measureDatetime.push_back(at(b.measureDatetime, r, std::string()));
            net.measureEditLock.push_back(at(b.measureEditLock, r) != 0); net.hasMeasureEditLock.push_back(at(b.hasMeasureEditLock, r) != 0);
            net.measureIgnore.push_back(at(b.measureIgnore, r) != 0); net.hasMeasureIgnore.push_back(at(b.hasMeasureIgnore, r) != 0);
            net.measureJigsawRejected.push_back(at(b.measureJigsawRejected, r) != 0); net.hasMeasureJigsawRejected.push_back(at(b.hasMeasureJigsawRejected, r) != 0);
            net.diameter.push_back(at(b.diameter, r)); net.hasDiameter.push_back(at(b.hasDiameter, r));
            net.aprioriSample.push_back(at(b.aprioriSample, r)); net.hasAprioriSample.push_back(at(b.hasAprioriSample, r));
            net.aprioriLine.push_back(at(b.aprioriLine, r)); net.hasAprioriLine.push_back(at(b.hasAprioriLine, r));
            net.sampleSigma.push_back(at(b.sampleSigma, r)); net.hasSampleSigma.push_back(at(b.hasSampleSigma, r));
            net.lineSigma.push_back(at(b.lineSigma, r)); net.hasLineSigma.push_back(at(b.hasLineSigma, r));
            for (int32_t k = 0; k < logLen && logPos < b.measureLogType.size(); ++k) {
                net.measureLogType.push_back(b.measureLogType[logPos]);
                net.measureLogValue.push_back(logPos < b.measureLogValue.size() ? b.measureLogValue[logPos] : 0.0);
                ++logPos;
            }
            net.measureLogOffset.push_back(static_cast<uint32_t>(net.measureLogType.size()));
            net.measureCount.back() += 1;
        } else {
            logPos += logLen;  // keep the flattened log cursor aligned
        }
    }
    return net;
}

}  // namespace cnet
