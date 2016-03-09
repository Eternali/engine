// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"
#include "base/values.h"
#include "base/json/json_reader.h"
#include "services/asset_bundle/zip_asset_bundle.h"
#include "sky/engine/core/script/ui_dart_state.h"
#include "sky/engine/platform/fonts/FontData.h"
#include "sky/engine/platform/fonts/FontFaceCreationParams.h"
#include "sky/engine/platform/fonts/SimpleFontData.h"
#include "sky/shell/ui/flutter_font_selector.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkFontMgr.h"

namespace sky {
namespace shell {

using base::DictionaryValue;
using base::JSONReader;
using base::ListValue;
using base::StringValue;
using base::Value;
using blink::FontCacheKey;
using blink::FontData;
using blink::FontDescription;
using blink::FontFaceCreationParams;
using blink::FontPlatformData;
using blink::FontStyle;
using blink::FontWeight;
using blink::SimpleFontData;
using mojo::asset_bundle::ZipAssetBundle;

// Style attributes of a Flutter font asset.
struct FlutterFontSelector::FlutterFontAttributes {
  FlutterFontAttributes(const std::string& path);
  ~FlutterFontAttributes();
  std::string asset_path;
  int weight;
  FontStyle style;
};

// A Skia typeface along with a buffer holding the raw typeface asset data.
struct FlutterFontSelector::TypefaceAsset {
  TypefaceAsset();
  ~TypefaceAsset();
  RefPtr<SkTypeface> typeface;
  std::vector<uint8_t> data;
};

namespace {
const char kFontManifestAssetPath[] = "FontManifest.json";

// Weight values corresponding to the members of the FontWeight enum.
const int kFontWeightValue[] = {100, 200, 300, 400, 500, 600, 700, 800, 900};

const int kFontWeightNormal = kFontWeightValue[FontWeight::FontWeightNormal];

int getFontWeightValue(FontWeight weight) {
  size_t weight_index = weight;
  return (weight_index < arraysize(kFontWeightValue)) ?
      kFontWeightValue[weight_index] : kFontWeightNormal;
}

// Compares fonts within a family to determine which one most closely matches
// a FontDescription.
struct FontMatcher {
  using FlutterFontAttributes = FlutterFontSelector::FlutterFontAttributes;

  FontMatcher(const FontDescription& description)
      : description_(description),
        target_weight_(getFontWeightValue(description.weight())) {
  }

  bool operator()(const FlutterFontAttributes& font1,
                  const FlutterFontAttributes& font2) {
    if (font1.style != font2.style) {
      if (font1.style == description_.style())
        return true;
      if (font2.style == description_.style())
        return false;
    }

    int weight_delta1 = abs(font1.weight - target_weight_);
    int weight_delta2 = abs(font2.weight - target_weight_);
    return weight_delta1 < weight_delta2;
  }

 private:
  const FontDescription& description_;
  int target_weight_;
};

}

void FlutterFontSelector::install(
    const scoped_refptr<ZipAssetBundle>& zip_asset_bundle) {
  RefPtr<FlutterFontSelector> font_selector = adoptRef(
      new FlutterFontSelector(zip_asset_bundle));
  font_selector->parseFontManifest();
  blink::UIDartState::Current()->set_font_selector(font_selector);
}

FlutterFontSelector::FlutterFontSelector(
    const scoped_refptr<ZipAssetBundle>& zip_asset_bundle)
    : zip_asset_bundle_(zip_asset_bundle) {
}

FlutterFontSelector::~FlutterFontSelector() {
}

FlutterFontSelector::TypefaceAsset::TypefaceAsset() {
}

FlutterFontSelector::TypefaceAsset::~TypefaceAsset() {
}

FlutterFontSelector::FlutterFontAttributes::FlutterFontAttributes(
    const std::string& path)
    : asset_path(path),
      weight(kFontWeightNormal),
      style(FontStyle::FontStyleNormal) {
}

FlutterFontSelector::FlutterFontAttributes::~FlutterFontAttributes() {
}

void FlutterFontSelector::parseFontManifest() {
  std::vector<uint8_t> font_manifest_data;
  if (!zip_asset_bundle_->GetAsBuffer(kFontManifestAssetPath,
                                      &font_manifest_data))
    return;

  base::StringPiece font_manifest_str(
      reinterpret_cast<const char*>(font_manifest_data.data()),
      font_manifest_data.size());
  scoped_ptr<Value> font_manifest_json = JSONReader::Read(font_manifest_str);
  if (font_manifest_json == nullptr)
    return;

  ListValue* family_list;
  if (!font_manifest_json->GetAsList(&family_list))
    return;

  for (auto family : *family_list) {
    DictionaryValue* family_dict;
    if (!family->GetAsDictionary(&family_dict))
      continue;
    std::string family_name;
    if (!family_dict->GetString("family", &family_name))
      continue;

    ListValue* font_list;
    if (!family_dict->GetList("fonts", &font_list))
      continue;

    AtomicString family_key = AtomicString::fromUTF8(family_name.c_str());
    auto set_result = font_family_map_.set(
        family_key, std::vector<FlutterFontAttributes>());
    std::vector<FlutterFontAttributes>& family_assets =
        set_result.storedValue->value;

    for (Value* list_entry : *font_list) {
      DictionaryValue* font_dict;
      if (!list_entry->GetAsDictionary(&font_dict))
        continue;

      std::string asset_path;
      if (!font_dict->GetString("asset", &asset_path))
        continue;

      FlutterFontAttributes attributes(asset_path);
      font_dict->GetInteger("weight", &attributes.weight);

      std::string style;
      if (font_dict->GetString("style", &style)) {
        if (style == "italic")
          attributes.style = FontStyle::FontStyleItalic;
      }

      family_assets.push_back(attributes);
    }
  }
}

PassRefPtr<FontData> FlutterFontSelector::getFontData(
    const FontDescription& font_description,
    const AtomicString& family_name) {
  FontFaceCreationParams creationParams(family_name);
  FontCacheKey key = font_description.cacheKey(creationParams);
  RefPtr<SimpleFontData> font_data = font_platform_data_cache_.get(key);

  if (font_data == nullptr) {
    SkTypeface* typeface = getTypefaceAsset(font_description, family_name);
    if (typeface == nullptr)
      return nullptr;

    bool synthetic_bold =
        (font_description.weight() >= blink::FontWeight600 && !typeface->isBold()) ||
        font_description.isSyntheticBold();
    bool synthetic_italic =
        (font_description.style() && !typeface->isItalic()) ||
        font_description.isSyntheticItalic();
    FontPlatformData platform_data(
        typeface,
        family_name.utf8().data(),
        font_description.effectiveFontSize(),
        synthetic_bold,
        synthetic_italic,
        font_description.orientation(),
        font_description.useSubpixelPositioning());

    font_data = SimpleFontData::create(platform_data);
    font_platform_data_cache_.set(key, font_data);
  }

  return font_data;
}

SkTypeface* FlutterFontSelector::getTypefaceAsset(
    const FontDescription& font_description,
    const AtomicString& family_name) {
  auto family_iter = font_family_map_.find(family_name);
  if (family_iter == font_family_map_.end())
    return nullptr;

  const std::vector<FlutterFontAttributes>& fonts = family_iter->value;
  if (fonts.empty())
    return nullptr;

  std::vector<FlutterFontAttributes>::const_iterator font_iter;
  if (fonts.size() == 1) {
    font_iter = fonts.begin();
  } else {
    font_iter = std::min_element(fonts.begin(), fonts.end(),
                                 FontMatcher(font_description));
  }

  const std::string& asset_path = font_iter->asset_path;
  auto typeface_iter = typeface_cache_.find(asset_path);
  if (typeface_iter != typeface_cache_.end()) {
    const TypefaceAsset* cache_asset = typeface_iter->second.get();
    return cache_asset ? cache_asset->typeface.get() : nullptr;
  }

  std::unique_ptr<TypefaceAsset> typeface_asset(new TypefaceAsset);
  if (!zip_asset_bundle_->GetAsBuffer(asset_path,
                                      &typeface_asset->data)) {
    typeface_cache_.insert(std::make_pair(asset_path, nullptr));
    return nullptr;
  }

  SkAutoTUnref<SkFontMgr> font_mgr(SkFontMgr::RefDefault());
  SkMemoryStream* typeface_stream = new SkMemoryStream(
      typeface_asset->data.data(), typeface_asset->data.size());
  typeface_asset->typeface = adoptRef(
      font_mgr->createFromStream(typeface_stream));
  if (typeface_asset->typeface == nullptr) {
    typeface_cache_.insert(std::make_pair(asset_path, nullptr));
    return nullptr;
  }

  SkTypeface* result = typeface_asset->typeface.get();
  typeface_cache_.insert(std::make_pair(
      asset_path, std::move(typeface_asset)));

  return result;
}

void FlutterFontSelector::willUseFontData(
    const FontDescription& font_description,
    const AtomicString& family,
    UChar32 character) {
}

unsigned FlutterFontSelector::version() const {
  return 0;
}

void FlutterFontSelector::fontCacheInvalidated() {
}

}  // namespace shell
}  // namespace sky