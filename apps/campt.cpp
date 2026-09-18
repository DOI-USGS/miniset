#include "csm/campt.hpp"
#include "csm/csm_interface.hpp"
#include "utils/coordinate_transforms.hpp"
#include "../external/argparse.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>

// Get ellipsoid from CSM sensor model
Ellipsoid3 get_ellipsoid_from_sensor(const ::csm::RasterGM* sensor) {
#ifdef MINISET_HAS_CSMAPI
    auto radii = csm::getRadii(sensor);
    double a = radii.first;   // Semi-major
    double c = radii.second;  // Semi-minor
    return Ellipsoid3(a, a, c);  // Assume a = b (spheroid)
#else
    return Ellipsoid3(3396190.0, 3396190.0, 3376200.0);  // Mars fallback
#endif
}

void output_pvl(const csm::CamptInfo& info, const std::string& filename) {
    auto rad2deg = [](double rad) { return rad * 180.0 / M_PI; };
    auto normalize_lon_360 = [](double lon) {
        double deg = lon * 180.0 / M_PI;
        while (deg < 0) deg += 360.0;
        while (deg >= 360.0) deg -= 360.0;
        return deg;
    };
    auto normalize_lon_180 = [](double lon) {
        double deg = lon * 180.0 / M_PI;
        while (deg < -180.0) deg += 360.0;
        while (deg >= 180.0) deg -= 360.0;
        return deg;
    };

    std::cout << std::fixed << std::setprecision(12);
    std::cout << "Group = GroundPoint\n";
    std::cout << "  Filename                   = " << filename << "\n";
    std::cout << "  Sample                     = " << info.sample << "\n";
    std::cout << "  Line                       = " << info.line << "\n";
    std::cout << "  PixelValue                 = NULL\n";

    // Celestial coordinates
    std::cout << "  RightAscension             = " << rad2deg(info.right_ascension) << " <DEGREE>\n";
    std::cout << "  Declination                = " << rad2deg(info.declination) << " <DEGREE>\n";

    // Latitude variants
    std::cout << "  PlanetocentricLatitude     = " << rad2deg(info.planetocentric_lat) << " <DEGREE>\n";
    std::cout << "  PlanetographicLatitude     = " << rad2deg(info.planetographic_lat) << " <DEGREE>\n";

    // Longitude variants
    double lon_e360 = normalize_lon_360(info.positive_east_lon);
    double lon_e180 = normalize_lon_180(info.positive_east_lon);
    std::cout << "  PositiveEast360Longitude   = " << lon_e360 << " <DEGREE>\n";
    std::cout << "  PositiveEast180Longitude   = " << lon_e180 << " <DEGREE>\n";
    std::cout << "  PositiveWest360Longitude   = " << (360.0 - lon_e360) << " <DEGREE>\n";
    std::cout << "  PositiveWest180Longitude   = " << (-lon_e180) << " <DEGREE>\n";

    // Body-fixed coordinate (km)
    std::cout << "  BodyFixedCoordinate        = (" << info.ground_point.x / 1000.0 << ", "
              << info.ground_point.y / 1000.0 << ",\n"
              << "                                " << info.ground_point.z / 1000.0 << ") <km>\n";
    std::cout << "  LocalRadius                = " << info.local_radius << " <meters>\n";

    // Resolution
    std::cout << "  SampleResolution           = " << info.sample_resolution << " <meters/pixel>\n";
    std::cout << "  LineResolution             = " << info.line_resolution << " <meters/pixel>\n";
    std::cout << "  ObliqueDetectorResolution  = " << info.oblique_resolution << " <meters>\n";
    std::cout << "  ObliquePixelResolution     = " << info.oblique_resolution << " <meters/pix>\n";
    std::cout << "  ObliqueLineResolution      = " << info.oblique_resolution << " <meters>\n";
    std::cout << "  ObliqueSampleResolution    = " << info.oblique_resolution << " <meters>\n";

    // Spacecraft Information
    std::cout << "\n  # Spacecraft Information\n";
    std::cout << "  SpacecraftPosition         = (" << info.sensor_position.x / 1000.0 << ", "
              << info.sensor_position.y / 1000.0 << ",\n"
              << "                                " << info.sensor_position.z / 1000.0 << ") <km>\n";
    std::cout << "  SpacecraftAzimuth          = " << info.spacecraft_azimuth << " <DEGREE>\n";
    std::cout << "  SlantDistance              = " << info.slant_distance / 1000.0 << " <km>\n";
    std::cout << "  TargetCenterDistance       = " << info.target_center_distance / 1000.0 << " <km>\n";
    std::cout << "  SubSpacecraftLatitude      = " << rad2deg(info.sub_spacecraft_lat) << " <DEGREE>\n";
    std::cout << "  SubSpacecraftLongitude     = " << normalize_lon_360(info.sub_spacecraft_lon) << " <DEGREE>\n";
    std::cout << "  SpacecraftAltitude         = " << info.spacecraft_altitude / 1000.0 << " <km>\n";
    std::cout << "  OffNadirAngle              = " << info.off_nadir_angle << " <DEGREE>\n";
    std::cout << "  SubSpacecraftGroundAzimuth = " << info.sub_spacecraft_ground_azimuth << " <DEGREE>\n";

    // Sun Information
    std::cout << "\n  # Sun Information\n";
    if (info.solar_distance > 0) {
        std::cout << "  SunPosition                = (" << info.sun_position.x / 1000.0 << ", "
                  << info.sun_position.y / 1000.0 << ",\n"
                  << "                                " << info.sun_position.z / 1000.0 << ") <km>\n";
        std::cout << "  SubSolarAzimuth            = " << info.sub_solar_azimuth << " <DEGREE>\n";
        std::cout << "  SolarDistance              = " << info.solar_distance << " <AU>\n";
        std::cout << "  SubSolarLatitude           = " << rad2deg(info.sub_solar_lat) << " <DEGREE>\n";
        std::cout << "  SubSolarLongitude          = " << normalize_lon_360(info.sub_solar_lon) << " <DEGREE>\n";
        std::cout << "  SubSolarGroundAzimuth      = " << info.sub_solar_ground_azimuth << " <DEGREE>\n";
    } else {
        std::cout << "  SunPosition                = NULL\n";
        std::cout << "  SubSolarAzimuth            = NULL\n";
        std::cout << "  SolarDistance              = NULL\n";
        std::cout << "  SubSolarLatitude           = NULL\n";
        std::cout << "  SubSolarLongitude          = NULL\n";
        std::cout << "  SubSolarGroundAzimuth      = NULL\n";
    }

    // Illumination and Other
    std::cout << "\n  # Illumination and Other\n";
    std::cout << "  Phase                      = " << info.phase_angle << " <DEGREE>\n";
    std::cout << "  Incidence                  = " << info.incidence_angle << " <DEGREE>\n";
    std::cout << "  Emission                   = " << info.emission_angle << " <DEGREE>\n";
    std::cout << "  NorthAzimuth               = " << info.north_azimuth << " <DEGREE>\n";

    // Time
    std::cout << "\n  # Time\n";
    std::cout << "  EphemerisTime              = " << info.sensor_time << " <seconds>\n";
    std::cout << "  UTC                        = NULL\n";
    if (info.local_solar_time > 0) {
        std::cout << "  LocalSolarTime             = " << info.local_solar_time << " <hour>\n";
        std::cout << "  SolarLongitude             = " << rad2deg(info.solar_longitude) << " <DEGREE>\n";
    } else {
        std::cout << "  LocalSolarTime             = NULL\n";
        std::cout << "  SolarLongitude             = NULL\n";
    }

    // Look Direction
    std::cout << "\n  # Look Direction Unit Vectors in Body Fixed, J2000, and Camera Coordinate Systems.\n";
    std::cout << "  LookDirectionBodyFixed     = (" << info.look_direction.x << ", "
              << info.look_direction.y << ",\n"
              << "                                " << info.look_direction.z << ")\n";
    if (info.look_j2000.x != 0 || info.look_j2000.y != 0 || info.look_j2000.z != 0) {
        std::cout << "  LookDirectionJ2000         = (" << info.look_j2000.x << ", "
                  << info.look_j2000.y << ",\n"
                  << "                                " << info.look_j2000.z << ")\n";
        std::cout << "  LookDirectionCamera        = (" << info.look_camera.x << ", "
                  << info.look_camera.y << ",\n"
                  << "                                " << info.look_camera.z << ")\n";
    } else {
        std::cout << "  LookDirectionJ2000         = NULL\n";
        std::cout << "  LookDirectionCamera        = NULL\n";
    }

    std::cout << "End_Group\n";
}

void output_json(const csm::CamptInfo& info, const std::string& filename) {
    auto rad2deg = [](double rad) { return rad * 180.0 / M_PI; };

    std::cout << std::fixed << std::setprecision(12);
    std::cout << "{\n";
    std::cout << "  \"Filename\": \"" << filename << "\",\n";
    std::cout << "  \"Sample\": " << info.sample << ",\n";
    std::cout << "  \"Line\": " << info.line << ",\n";
    std::cout << "  \"PlanetocentricLatitude\": " << rad2deg(info.planetocentric_lat) << ",\n";
    std::cout << "  \"PlanetographicLatitude\": " << rad2deg(info.planetographic_lat) << ",\n";
    std::cout << "  \"Longitude\": " << rad2deg(info.positive_east_lon) << ",\n";
    std::cout << "  \"BodyFixedCoordinate\": [" << info.ground_point.x / 1000.0 << ", "
              << info.ground_point.y / 1000.0 << ", " << info.ground_point.z / 1000.0 << "],\n";
    std::cout << "  \"LocalRadius\": " << info.local_radius << ",\n";
    std::cout << "  \"SampleResolution\": " << info.sample_resolution << ",\n";
    std::cout << "  \"LineResolution\": " << info.line_resolution << ",\n";
    std::cout << "  \"SpacecraftPosition\": [" << info.sensor_position.x / 1000.0 << ", "
              << info.sensor_position.y / 1000.0 << ", " << info.sensor_position.z / 1000.0 << "],\n";
    std::cout << "  \"SlantDistance\": " << info.slant_distance / 1000.0 << ",\n";
    std::cout << "  \"TargetCenterDistance\": " << info.target_center_distance / 1000.0 << ",\n";
    std::cout << "  \"SubSpacecraftLatitude\": " << rad2deg(info.sub_spacecraft_lat) << ",\n";
    std::cout << "  \"SubSpacecraftLongitude\": " << rad2deg(info.sub_spacecraft_lon) << ",\n";
    std::cout << "  \"SpacecraftAltitude\": " << info.spacecraft_altitude / 1000.0 << ",\n";
    std::cout << "  \"OffNadirAngle\": " << info.off_nadir_angle << ",\n";
    std::cout << "  \"Phase\": " << info.phase_angle << ",\n";
    std::cout << "  \"Incidence\": " << info.incidence_angle << ",\n";
    std::cout << "  \"Emission\": " << info.emission_angle << ",\n";
    std::cout << "  \"NorthAzimuth\": " << info.north_azimuth << ",\n";
    std::cout << "  \"EphemerisTime\": " << info.sensor_time << ",\n";
    std::cout << "  \"LookDirectionBodyFixed\": [" << info.look_direction.x << ", "
              << info.look_direction.y << ", " << info.look_direction.z << "]\n";
    std::cout << "}\n";
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("campt", "1.0");

    program.add_description("Compute camera point information from CSM sensor models");

    program.add_argument("isd_file")
        .help("Path to CSM ISD JSON file");

    program.add_argument("--format", "-f")
        .default_value(std::string("json"))
        .choices("json", "pvl", "text")
        .help("Output format (default: json)");

    program.add_argument("--latlon", "-l")
        .default_value(false)
        .implicit_value(true)
        .help("Reverse mode: compute image coordinates from lat/lon");

    program.add_argument("sample_or_lat")
        .scan<'g', double>()
        .help("Sample coordinate (pixels) or Latitude (degrees) if --latlon");

    program.add_argument("line_or_lon")
        .scan<'g', double>()
        .help("Line coordinate (pixels) or Longitude (degrees) if --latlon");

    program.add_argument("height")
        .scan<'g', double>()
        .default_value(0.0)
        .nargs(argparse::nargs_pattern::optional)
        .help("Height above ellipsoid (meters, default: 0)");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    std::string isd_file = program.get<std::string>("isd_file");
    std::string output_format = program.get<std::string>("--format");
    bool reverse_mode = program.get<bool>("--latlon");
    double param1 = program.get<double>("sample_or_lat");
    double param2 = program.get<double>("line_or_lon");
    double height = program.get<double>("height");

    try {
#ifdef MINISET_HAS_CSMAPI
        ::csm::RasterGM* sensor = csm::createCsmFromISD(isd_file);
        if (!sensor) {
            std::cerr << "Error: Failed to create CSM sensor from ISD file: " << isd_file << "\n";
            return 1;
        }

        Ellipsoid3 ellipsoid = get_ellipsoid_from_sensor(sensor);

        csm::CamptInfo info;

        if (reverse_mode) {
            // Reverse: lat/lon -> image
            double lat_rad = param1 * M_PI / 180.0;
            double lon_rad = param2 * M_PI / 180.0;

            Vec3 ecef_pt = utils::latLonToEcef(lat_rad, lon_rad, height, ellipsoid.a, ellipsoid.c);
            ::csm::EcefCoord ground_pt(ecef_pt.x, ecef_pt.y, ecef_pt.z);
            ::csm::ImageCoord img_coord = sensor->groundToImage(ground_pt);

            info = csm::campt(sensor, img_coord.samp, img_coord.line, ellipsoid, height);
        } else {
            // Forward: image -> lat/lon
            info = csm::campt(sensor, param1, param2, ellipsoid, height);
        }

        // Output based on format
        if (output_format == "pvl") {
            output_pvl(info, isd_file);
        } else if (output_format == "json") {
            output_json(info, isd_file);
        } else {
            // Text format
            std::cout << std::fixed << std::setprecision(8);
            std::cout << "\n=== Camera Point Information ===\n\n";
            std::cout << "Input:\n";
            std::cout << "  Sample:                 " << info.sample << " pixels\n";
            std::cout << "  Line:                   " << info.line << " pixels\n";
            std::cout << "  Height:                 " << info.height << " m\n";
            std::cout << "\n";
            std::cout << "Ground Point (ECEF):\n";
            std::cout << "  X:                      " << info.ground_point.x << " m\n";
            std::cout << "  Y:                      " << info.ground_point.y << " m\n";
            std::cout << "  Z:                      " << info.ground_point.z << " m\n";
            std::cout << "\n";
            std::cout << "Ground Point (Geodetic):\n";
            std::cout << "  Latitude:               " << (info.planetocentric_lat * 180.0 / M_PI) << " deg\n";
            std::cout << "  Longitude:              " << (info.positive_east_lon * 180.0 / M_PI) << " deg\n";
            std::cout << "  Radius:                 " << info.local_radius << " m\n";
            std::cout << "\n";
            std::cout << "Sensor Position (ECEF):\n";
            std::cout << "  X:                      " << info.sensor_position.x << " m\n";
            std::cout << "  Y:                      " << info.sensor_position.y << " m\n";
            std::cout << "  Z:                      " << info.sensor_position.z << " m\n";
            std::cout << "  Time:                   " << info.sensor_time << " s\n";
            std::cout << "\n";
            std::cout << "Look Direction:\n";
            std::cout << "  X:                      " << info.look_direction.x << "\n";
            std::cout << "  Y:                      " << info.look_direction.y << "\n";
            std::cout << "  Z:                      " << info.look_direction.z << "\n";
            std::cout << "\n";
            std::cout << "Photometric Angles:\n";
            std::cout << "  Phase Angle:            " << info.phase_angle << " deg\n";
            std::cout << "  Emission Angle:         " << info.emission_angle << " deg\n";
            std::cout << "  Incidence Angle:        " << info.incidence_angle << " deg\n";
            std::cout << "\n";
            std::cout << "Resolution:\n";
            std::cout << "  Sample Resolution:      " << info.sample_resolution << " m/pixel\n";
            std::cout << "  Line Resolution:        " << info.line_resolution << " m/pixel\n";
            std::cout << "  Pixel Resolution:       " << info.pixel_resolution << " m/pixel\n";
            std::cout << "\n";
            std::cout << "Distances:\n";
            std::cout << "  Slant Distance:         " << info.slant_distance << " m\n";
            std::cout << "  Target Center Distance: " << info.target_center_distance << " m\n";
            std::cout << "\n";
            std::cout << "Sub-Spacecraft Point:\n";
            std::cout << "  Latitude:               " << (info.sub_spacecraft_lat * 180.0 / M_PI) << " deg\n";
            std::cout << "  Longitude:              " << (info.sub_spacecraft_lon * 180.0 / M_PI) << " deg\n";
            std::cout << "\n";
        }

        delete sensor;
        return 0;
#else
        std::cerr << "Error: miniset was not built with CSM support\n";
        return 1;
#endif

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
