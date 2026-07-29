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

#ifndef MINISET_CNET_COLUMN_SOURCE_HPP
#define MINISET_CNET_COLUMN_SOURCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "cnet/control_net.hpp"

namespace cnet {

/// Denormalized column batch — one entry per Parquet row (row-per-measure),
/// mirroring ISIS's Parquet schema. This is the format-agnostic hand-off used by
/// the WASM path: a JS Parquet reader fills these column vectors and the C++
/// core assembles a ControlNet, so no Parquet/Arrow code is compiled to WASM.
///
/// Every column is sized to the row count. Optional numeric columns carry a
/// parallel `has*` vector (0/1); string columns use empty-string for absent.
/// Rows for one point are contiguous and share the same `id`.
struct ColumnBatch {
    // Header (row 0 is authoritative; repeated on every row).
    std::string networkId, targetName, created, lastModified, description, userName;

    // Point columns.
    std::vector<std::string> id;
    std::vector<int32_t> type;
    std::vector<std::string> chooserName, datetime;
    std::vector<int32_t> editLock, hasEditLock, ignore, hasIgnore,
                         jigsawRejected, hasJigsawRejected, referenceIndex, hasReferenceIndex;
    std::vector<int32_t> aprioriSurfPointSource, aprioriRadiusSource;
    std::vector<std::string> aprioriSurfPointSourceFile, aprioriRadiusSourceFile;
    std::vector<double> aprioriX, aprioriY, aprioriZ, hasApriori;
    std::vector<double> adjustedX, adjustedY, adjustedZ, hasAdjusted;
    // Covariance list columns: flattened values + per-row length.
    std::vector<double> aprioriCovar; std::vector<int32_t> aprioriCovarLen;
    std::vector<double> adjustedCovar; std::vector<int32_t> adjustedCovarLen;

    // Measure columns. A row with empty serialnumber is a zero-measure point row.
    std::vector<std::string> serialnumber;
    std::vector<int32_t> measureType;
    std::vector<double> sample, hasSample, line, hasLine,
                        sampleResidual, hasSampleResidual, lineResidual, hasLineResidual;
    std::vector<std::string> measureChooserName, measureDatetime;
    std::vector<int32_t> measureEditLock, hasMeasureEditLock, measureIgnore, hasMeasureIgnore,
                         measureJigsawRejected, hasMeasureJigsawRejected;
    std::vector<double> diameter, hasDiameter, aprioriSample, hasAprioriSample,
                        aprioriLine, hasAprioriLine, sampleSigma, hasSampleSigma,
                        lineSigma, hasLineSigma;
    std::vector<int32_t> measureLogType; std::vector<double> measureLogValue;
    std::vector<int32_t> measureLogLen;  // per-row log entry count

    size_t numRows() const { return id.size(); }
};

/// Assemble a ControlNet from a denormalized ColumnBatch, grouping contiguous
/// rows by `id` into points (matching the Parquet reader semantics). Portable —
/// used by the WASM JS-fed path and unit-testable natively.
ControlNet control_net_from_columns(const ColumnBatch& batch);

}  // namespace cnet

#endif  // MINISET_CNET_COLUMN_SOURCE_HPP
