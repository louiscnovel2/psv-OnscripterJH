#pragma once

#include <vitasdk.h>
#include <iostream>
#define PACKAGE_TEMP_FOLDER std::string("ux0:/temp/pkg/")

class VitaPackage{
public:
  VitaPackage(const std::string path);

  ~VitaPackage();
  //TITLE=STITLE TITLE_ID
  int SetSFOString(std::string title, std::string title_id);
  int Install();
  //int Install();
  // Always ends with a '/' (e.g. "ux0:/temp/pkg/")
  static std::string Package_path;
private:
  // Never ends with a '/' (scePromoterUtilityPromotePkg() requires it)
  std::string path_;
};
