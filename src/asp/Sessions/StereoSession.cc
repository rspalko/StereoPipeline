// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__


/// \file StereoSession.cc
///
#include <vw/Core/Exception.h>
#include <vw/Core/Log.h>
#include <vw/Math/Vector.h>
#include <vw/Image/ImageViewRef.h>
#include <vw/Image/PixelMask.h>
#include <vw/Image/PixelTypeInfo.h>
#include <vw/FileIO/DiskImageResource.h>
#include <vw/FileIO/DiskImageView.h>

#include <asp/Core/StereoSettings.h>
#include <asp/Core/Common.h>

#include <asp/Sessions/StereoSession.h>
#include <asp/Core/InterestPointMatching.h>
#include <asp/Core/BundleAdjustUtils.h>

#include <map>
#include <utility>
#include <string>
#include <ostream>
#include <limits>

using namespace vw;


/// Allows FileIO to correctly read/write these pixel types
namespace vw {
  template<> struct PixelFormatID<Vector3>   { static const PixelFormatEnum value = VW_PIXEL_GENERIC_3_CHANNEL; };
}

namespace asp {

  // Pass over all the string variables we use
  void StereoSession::initialize( BaseOptions const& options,
                                  std::string const& left_image_file,
                                  std::string const& right_image_file,
                                  std::string const& left_camera_file,
                                  std::string const& right_camera_file,
                                  std::string const& out_prefix,
                                  std::string const& input_dem) {
    m_options           = options;
    m_left_image_file   = left_image_file;
    m_right_image_file  = right_image_file;
    m_left_camera_file  = left_camera_file;
    m_right_camera_file = right_camera_file;
    m_out_prefix        = out_prefix;
    m_input_dem         = input_dem;
  }

  // A default IP matching implementation that derived classes can use
  bool StereoSession::ip_matching(std::string const& input_file1,
                                  std::string const& input_file2,
                                  int ip_per_tile,
                                  float nodata1, float nodata2,
                                  std::string const& match_filename,
                                  vw::camera::CameraModel* cam1,
                                  vw::camera::CameraModel* cam2){

    bool crop_left_and_right =
      ( stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0)) &&
      ( stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0) );

    // If we crop the images we must always create new matching files
    if (!crop_left_and_right && boost::filesystem::exists(match_filename)) {
      vw_out() << "\t--> Using cached match file: " << match_filename << "\n";
      return true;
    }

    DiskImageView<float> image1(input_file1), image2(input_file2);

    const bool nadir_facing = this->is_nadir_facing();

    bool inlier = false;
    if (nadir_facing) {
      // Run an IP matching function that takes the camera and datum info into account
      bool single_threaded_camera = true; // TODO: Does this make sense?
      cartography::Datum datum = this->get_datum(cam1);
      inlier = ip_matching_w_alignment(single_threaded_camera, cam1, cam2,
                                       image1, image2,
                                       ip_per_tile,
                                       datum, match_filename,
                                       nodata1, nodata2);
    } else { // Not nadir facing
      // Run a simpler purely image based matching function
      inlier = homography_ip_matching( image1, image2,
                                       ip_per_tile,
                                       match_filename,
                                       nodata1, nodata2);
    }
    if (!inlier) {
      boost::filesystem::remove(match_filename);
      vw_throw(IOErr() << "Unable to match left and right images.");
    }
    return inlier;
  }



  // Default implementation of this function.  Derived classes will probably override this.
  void StereoSession::camera_models(boost::shared_ptr<vw::camera::CameraModel> &cam1,
                                    boost::shared_ptr<vw::camera::CameraModel> &cam2) {
    cam1 = camera_model(m_left_image_file,  m_left_camera_file);
    cam2 = camera_model(m_right_image_file, m_right_camera_file);
  }

  // Processing Hooks. The default is to do nothing.
  void StereoSession::pre_preprocessing_hook(bool adjust_left_image_size,
                                             std::string const& input_file1,
                                             std::string const& input_file2,
                                             std::string      & output_file1,
                                             std::string      & output_file2) {
    output_file1 = input_file1;
    output_file2 = input_file2;
  }

  void StereoSession::post_preprocessing_hook(std::string const& input_file1,
                                              std::string const& input_file2,
                                              std::string &output_file1,
                                              std::string &output_file2) {
    output_file1 = input_file1;
    output_file2 = input_file2;
  }

  void StereoSession::pre_correlation_hook(std::string const& input_file1,
                                           std::string const& input_file2,
                                           std::string      & output_file1,
                                           std::string      & output_file2) {
    output_file1 = input_file1;
    output_file2 = input_file2;
  }

  void StereoSession::post_correlation_hook(std::string const& input_file,
                                            std::string      & output_file) {
    output_file = input_file;
  }

  void StereoSession::pre_filtering_hook(std::string const& input_file,
                                         std::string      & output_file) {
    output_file = input_file;
  }

  void StereoSession::post_filtering_hook(std::string const& input_file,
                                          std::string      & output_file) {
    output_file = input_file;
  }

  ImageViewRef<PixelMask<Vector2f> >
  StereoSession::pre_pointcloud_hook(std::string const& input_file) {
    return DiskImageView<PixelMask<Vector2f> >( input_file );
  }

  void StereoSession::post_pointcloud_hook(std::string const& input_file,
                                           std::string      & output_file) {
    output_file = input_file;
  }



  void StereoSession::get_nodata_values(boost::shared_ptr<vw::DiskImageResource> left_rsrc,
                                        boost::shared_ptr<vw::DiskImageResource> right_rsrc,
                                        float & left_nodata_value,
                                        float & right_nodata_value){

    // The no-data value read from options overrides the value present in the image files.
    left_nodata_value  = std::numeric_limits<float>::quiet_NaN();
    right_nodata_value = std::numeric_limits<float>::quiet_NaN();
    if (left_rsrc->has_nodata_read ()) left_nodata_value  = left_rsrc->nodata_read();
    if (right_rsrc->has_nodata_read()) right_nodata_value = right_rsrc->nodata_read();

    float opt_nodata = stereo_settings().nodata_value;
    if (!std::isnan(opt_nodata)){

      if ( opt_nodata < left_nodata_value )
        vw_out(WarningMessage) << "It appears that the user-supplied no-data value is less than the no-data value of left image. This may not be what was intended.\n";
      if ( opt_nodata < right_nodata_value )
        vw_out(WarningMessage) << "It appears that the user-supplied no-data value is less than the no-data value of right image. This may not be what was intended.\n";

      left_nodata_value  = opt_nodata;
      right_nodata_value = opt_nodata;
    }

    return;
  }

void StereoSession::shared_preprocessing_hook(asp::BaseOptions              & options,
                                              std::string const             & left_input_file,
                                              std::string const             & right_input_file,
                                              std::string                   & left_output_file,
                                              std::string                   & right_output_file,
                                              std::string                   & left_cropped_file,
                                              std::string                   & right_cropped_file,
                                              float                         & left_nodata_value,
                                              float                         & right_nodata_value,
                                              bool                          & has_left_georef,
                                              bool                          & has_right_georef,
                                              vw::cartography::GeoReference & left_georef,
                                              vw::cartography::GeoReference & right_georef){

  // Set output file paths
  left_output_file  = this->m_out_prefix + "-L.tif";
  right_output_file = this->m_out_prefix + "-R.tif";

  bool crop_left_and_right =
    ( stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0)) &&
    ( stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0) );

  // If the output files already exist, and we don't crop both left
  // and right images, then there is nothing to do here.
  if ( boost::filesystem::exists(left_output_file)  &&
       boost::filesystem::exists(right_output_file) &&
       (!crop_left_and_right)) {
    try {
      vw_log().console_log().rule_set().add_rule(-1,"fileio");
      DiskImageView<PixelGray<float32> > out_left (left_output_file );
      DiskImageView<PixelGray<float32> > out_right(right_output_file);
      vw_out(InfoMessage) << "\t--> Using cached normalized input images.\n";
      vw_settings().reload_config();
      return;
    } catch (vw::ArgumentErr const& e) {
      // This throws on a corrupted file.
      vw_settings().reload_config();
    } catch (vw::IOErr const& e) {
      vw_settings().reload_config();
    }
  } // End check for existing output files

  // Retrieve nodata values
  {
    // For this to work the ISIS type must be registered with the
    // DiskImageResource class.  - This happens in "stereo.cc", so
    // these calls will create DiskImageResourceIsis objects.
    boost::shared_ptr<DiskImageResource>
      left_rsrc (DiskImageResource::open(left_input_file )),
      right_rsrc(DiskImageResource::open(right_input_file));
    this->get_nodata_values(left_rsrc, right_rsrc,
                            left_nodata_value, right_nodata_value);
  }

  // Enforce no predictor in compression, it works badly with L.tif and R.tif.
  options = this->m_options;
  options.gdal_options["PREDICTOR"] = "1";

  left_cropped_file  = left_input_file;
  right_cropped_file = right_input_file;

  // See if to crop the images
  if (crop_left_and_right) {
    // Crop the images, will use them from now on. Crop the georef as
    // well, if available.
    left_cropped_file  = this->m_out_prefix + "-L-cropped.tif";
    right_cropped_file = this->m_out_prefix + "-R-cropped.tif";

    has_left_georef  = read_georeference(left_georef,  left_input_file);
    has_right_georef = read_georeference(right_georef, right_input_file);
    bool has_nodata = true;

    DiskImageView<float> left_orig_image(left_input_file);
    DiskImageView<float> right_orig_image(right_input_file);
    BBox2i left_win  = stereo_settings().left_image_crop_win;
    BBox2i right_win = stereo_settings().right_image_crop_win;
    left_win.crop (bounding_box(left_orig_image ));
    right_win.crop(bounding_box(right_orig_image));


    vw_out() << "\t--> Writing cropped image: " << left_cropped_file << "\n";
    block_write_gdal_image(left_cropped_file,
                           crop(left_orig_image, left_win),
                           has_left_georef, crop(left_georef, left_win),
                           has_nodata, left_nodata_value,
                           options,
                           TerminalProgressCallback("asp", "\t:  "));

    vw_out() << "\t--> Writing cropped image: " << right_cropped_file << "\n";
    block_write_gdal_image(right_cropped_file,
                           crop(right_orig_image, right_win),
                           has_right_georef,
                           crop(right_georef, right_win),
                           has_nodata, right_nodata_value,
                           options,
                           TerminalProgressCallback("asp", "\t:  "));
  }

  // Read the georef if available
  has_left_georef  = read_georeference(left_georef,  left_cropped_file);
  has_right_georef = read_georeference(right_georef, right_cropped_file);
  if ( stereo_settings().alignment_method != "none") {
    // If any alignment at all happens, the georef will be messed up.
    has_left_georef = false;
    has_right_georef = false;
  }
}

// TODO: Find a better place for these functions!

// If both left-image-crop-win and right-image-crop win are specified,
// we crop the images to these boxes, and hence the need to keep
// the upper-left corners of the crop windows to handle the cameras correctly.
vw::Vector2 camera_pixel_offset(std::string const& input_dem,
                                       std::string const& left_image_file,
                                       std::string const& right_image_file,
                                       std::string const& curr_image_file){

  // For map-projected images we don't apply a pixel offset.
  // When we need to do stereo on cropped images, we just
  // crop the images together with their georeferences.
  if (input_dem != "")
    return Vector2();

  vw::Vector2 left_pixel_offset, right_pixel_offset;
  if ( ( stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0)) &&
       ( stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0) ) ){
    left_pixel_offset  = stereo_settings().left_image_crop_win.min();
    right_pixel_offset = stereo_settings().right_image_crop_win.min();
  }
  if (curr_image_file == left_image_file)
    return left_pixel_offset;
  else if (curr_image_file == right_image_file)
    return right_pixel_offset;
  else
    vw_throw(ArgumentErr() << "Supplied image file does not match left or right image file.");

  return Vector2();
}

// If we have adjusted camera models, load them. The adjustment
// may be in the rotation matrix, camera center, or pixel offset.
boost::shared_ptr<vw::camera::CameraModel>
load_adjusted_model(boost::shared_ptr<vw::camera::CameraModel> cam,
                    std::string const& image_file,
                    std::string const& camera_file,
                    vw::Vector2 const& pixel_offset){

  // TODO: Where does this prefix come from?  Is it always an issue?
  std::string ba_pref = stereo_settings().bundle_adjust_prefix;
  if (ba_pref == "" && pixel_offset == vw::Vector2())
    return cam; // Just return if nothing is adjusting the camera

  Vector3 position_correction;
  Quaternion<double> pose_correction = Quat(math::identity_matrix<3>());

  if (ba_pref != "") {
    std::string adjust_file = asp::bundle_adjust_file_name(ba_pref, image_file, camera_file);
    if (boost::filesystem::exists(adjust_file)) {
      vw_out() << "Using adjusted camera model: " << adjust_file << std::endl;
      asp::read_adjustments(adjust_file, position_correction, pose_correction);
    }else {
      vw_throw(InputErr() << "Missing adjusted camera model: "
               << adjust_file << ".\n");
    }
  }

  return boost::shared_ptr<camera::CameraModel>(new vw::camera::AdjustedCameraModel(cam, position_correction, pose_correction, pixel_offset));
}

} // End namespace asp
