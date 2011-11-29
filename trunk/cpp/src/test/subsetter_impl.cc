/*
 * Copyright 2011 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// File is originally from Chromium third_party/sfntly/src/subsetter.
// Use as test case in sfntly so that problems can be caught in upstream early.

#include "test/subsetter_impl.h"

#include <string.h>

#include <map>
#include <set>

#include "sfntly/table/bitmap/eblc_table.h"
#include "sfntly/table/bitmap/ebdt_table.h"
#include "sfntly/table/bitmap/index_sub_table.h"
#include "sfntly/table/bitmap/index_sub_table_format1.h"
#include "sfntly/table/bitmap/index_sub_table_format2.h"
#include "sfntly/table/bitmap/index_sub_table_format3.h"
#include "sfntly/table/bitmap/index_sub_table_format4.h"
#include "sfntly/table/bitmap/index_sub_table_format5.h"
#include "sfntly/table/core/name_table.h"
#include "sfntly/table/truetype/glyph_table.h"
#include "sfntly/table/truetype/loca_table.h"
#include "sfntly/tag.h"
#include "sfntly/data/memory_byte_array.h"
#include "sfntly/port/memory_input_stream.h"
#include "sfntly/port/memory_output_stream.h"


namespace {

// The bitmap tables must be greater than 16KB to trigger bitmap subsetter.
static const int BITMAP_SIZE_THRESHOLD = 16384;

}

namespace sfntly {

void ConstructName(UChar* name_part, UnicodeString* name, int32_t name_id) {
  switch (name_id) {
    case NameId::kFullFontName:
      *name = name_part;
      break;
    case NameId::kFontFamilyName:
    case NameId::kPreferredFamily:
    case NameId::kWWSFamilyName: {
      UnicodeString original = *name;
      *name = name_part;
      *name += original;
      break;
    }
    case NameId::kFontSubfamilyName:
    case NameId::kPreferredSubfamily:
    case NameId::kWWSSubfamilyName:
      *name += name_part;
      break;
    default:
      // This name part is not used to construct font name (e.g. copyright).
      // Simply ignore it.
      break;
  }
}

int32_t HashCode(int32_t platform_id, int32_t encoding_id, int32_t language_id,
                 int32_t name_id) {
  int32_t result = platform_id << 24 | encoding_id << 16 | language_id << 8;
  if (name_id == NameId::kFullFontName) {
    result |= 0xff;
  } else if (name_id == NameId::kPreferredFamily ||
             name_id == NameId::kPreferredSubfamily) {
    result |= 0xf;
  } else if (name_id == NameId::kWWSFamilyName ||
             name_id == NameId::kWWSSubfamilyName) {
    result |= 1;
  }
  return result;
}

bool HasName(const char* font_name, Font* font) {
  UnicodeString font_string = UnicodeString::fromUTF8(font_name);
  if (font_string.isEmpty())
    return false;
  UnicodeString regular_suffix = UnicodeString::fromUTF8(" Regular");
  UnicodeString alt_font_string = font_string;
  alt_font_string += regular_suffix;

  typedef std::map<int32_t, UnicodeString> NameMap;
  NameMap names;
  NameTablePtr name_table = down_cast<NameTable*>(font->GetTable(Tag::name));
  if (name_table == NULL) {
    return false;
  }

  for (int32_t i = 0; i < name_table->NameCount(); ++i) {
    switch (name_table->NameId(i)) {
      case NameId::kFontFamilyName:
      case NameId::kFontSubfamilyName:
      case NameId::kFullFontName:
      case NameId::kPreferredFamily:
      case NameId::kPreferredSubfamily:
      case NameId::kWWSFamilyName:
      case NameId::kWWSSubfamilyName: {
        int32_t hash_code = HashCode(name_table->PlatformId(i),
                                     name_table->EncodingId(i),
                                     name_table->LanguageId(i),
                                     name_table->NameId(i));
        UChar* name_part = name_table->Name(i);
        ConstructName(name_part, &(names[hash_code]), name_table->NameId(i));
        delete[] name_part;
        break;
      }
      default:
        break;
    }
  }

  if (!names.empty()) {
    for (NameMap::iterator b = names.begin(), e = names.end(); b != e; ++b) {
      if (b->second.caseCompare(font_string, 0) == 0 ||
          b->second.caseCompare(alt_font_string, 0) == 0) {
        return true;
      }
    }
  }
  return false;
}

Font* FindFont(const char* font_name, const FontArray& font_array) {
  if (font_array.empty() || font_array[0] == NULL) {
    return NULL;
  }

  if (font_name && strlen(font_name)) {
    for (FontArray::const_iterator b = font_array.begin(), e = font_array.end();
         b != e; ++b) {
      if (HasName(font_name, (*b).p_)) {
        return (*b).p_;
      }
    }
  }

  return font_array[0].p_;
}

bool ResolveCompositeGlyphs(GlyphTable* glyf,
                            LocaTable* loca,
                            const unsigned int* glyph_ids,
                            size_t glyph_count,
                            IntegerSet* glyph_id_processed) {
  if (glyf == NULL || loca == NULL || glyph_ids == NULL || glyph_count == 0 ||
      glyph_id_processed == NULL) {
    return false;
  }

  // Find glyf and loca table.
  GlyphTablePtr glyph_table = glyf;
  LocaTablePtr loca_table = loca;

  // Sort and uniquify glyph ids.
  IntegerSet glyph_id_remaining;
  glyph_id_remaining.insert(0);  // Always include glyph id 0.
  for (size_t i = 0; i < glyph_count; ++i) {
    glyph_id_remaining.insert(glyph_ids[i]);
  }

  // Identify if any given glyph id maps to a composite glyph.  If so, include
  // the glyphs referenced by that composite glyph.
  while (!glyph_id_remaining.empty()) {
    IntegerSet comp_glyph_id;
    for (IntegerSet::iterator i = glyph_id_remaining.begin(),
                              e = glyph_id_remaining.end(); i != e; ++i) {
      if (*i < 0 || *i >= loca_table->num_glyphs()) {
        // Invalid glyph id, ignore.
        continue;
      }

      int32_t length = loca_table->GlyphLength(*i);
      if (length == 0) {
        // Empty glyph, ignore.
        continue;
      }
      int32_t offset = loca_table->GlyphOffset(*i);

      GlyphPtr glyph;
      glyph.Attach(glyph_table->GetGlyph(offset, length));
      if (glyph == NULL) {
        // Error finding glyph, ignore.
        continue;
      }

      if (glyph->GlyphType() == GlyphType::kComposite) {
        Ptr<GlyphTable::CompositeGlyph> comp_glyph =
            down_cast<GlyphTable::CompositeGlyph*>(glyph.p_);
        for (int32_t j = 0; j < comp_glyph->NumGlyphs(); ++j) {
          int32_t glyph_id = comp_glyph->GlyphIndex(j);
          if (glyph_id_processed->find(glyph_id) == glyph_id_processed->end() &&
              glyph_id_remaining.find(glyph_id) == glyph_id_remaining.end()) {
            comp_glyph_id.insert(comp_glyph->GlyphIndex(j));
          }
        }
      }

      glyph_id_processed->insert(*i);
    }

    glyph_id_remaining.clear();
    glyph_id_remaining = comp_glyph_id;
  }

  return true;
}

bool SetupGlyfBuilders(Font::Builder* builder,
                       GlyphTable* glyf,
                       LocaTable* loca,
                       const IntegerSet& glyph_ids) {
  if (!builder || !glyf || !loca) {
    return false;
  }

  // The tables are already checked in ResolveCompositeGlyphs().
  GlyphTablePtr glyph_table = glyf;
  LocaTablePtr loca_table = loca;

  FontBuilderPtr font_builder = builder;
  GlyphTableBuilderPtr glyph_table_builder =
      down_cast<GlyphTable::Builder*>(font_builder->NewTableBuilder(Tag::glyf));
  LocaTableBuilderPtr loca_table_builder =
      down_cast<LocaTable::Builder*>(font_builder->NewTableBuilder(Tag::loca));
  if (glyph_table_builder == NULL || loca_table_builder == NULL) {
    // Out of memory.
    return false;
  }

  // Extract glyphs and setup loca list.
  IntegerList loca_list;
  loca_list.resize(loca_table->num_glyphs());
  loca_list.push_back(0);
  int32_t last_glyph_id = 0;
  int32_t last_offset = 0;
  GlyphTable::GlyphBuilderList* glyph_builders =
      glyph_table_builder->GlyphBuilders();
  for (IntegerSet::const_iterator i = glyph_ids.begin(), e = glyph_ids.end();
                                  i != e; ++i) {
    int32_t length = loca_table->GlyphLength(*i);
    int32_t offset = loca_table->GlyphOffset(*i);

    GlyphPtr glyph;
    glyph.Attach(glyph_table->GetGlyph(offset, length));

    // Add glyph to new glyf table.
    ReadableFontDataPtr data = glyph->ReadFontData();
    WritableFontDataPtr copy_data;
    copy_data.Attach(WritableFontData::CreateWritableFontData(data->Length()));
    data->CopyTo(copy_data);
    GlyphBuilderPtr glyph_builder;
    glyph_builder.Attach(glyph_table_builder->GlyphBuilder(copy_data));
    glyph_builders->push_back(glyph_builder);

    // Configure loca list.
    for (int32_t j = last_glyph_id + 1; j <= *i; ++j) {
      loca_list[j] = last_offset;
    }
    last_offset += length;
    loca_list[*i + 1] = last_offset;
    last_glyph_id = *i;
  }
  for (int32_t j = last_glyph_id + 1; j <= loca_table->num_glyphs(); ++j) {
    loca_list[j] = last_offset;
  }
  loca_table_builder->SetLocaList(&loca_list);

  return true;
}

bool HasOverlap(int32_t range_begin, int32_t range_end,
                const IntegerSet& glyph_ids) {
  if (range_begin == range_end) {
    return glyph_ids.find(range_begin) != glyph_ids.end();
  } else if (range_end > range_begin) {
    IntegerSet::const_iterator left = glyph_ids.lower_bound(range_begin);
    IntegerSet::const_iterator right = glyph_ids.lower_bound(range_end);
    return right != left;
  }
  return false;
}

// Initialize builder, returns false if glyph_id subset is not covered.
bool ShallSubset(EbdtTable::Builder* ebdt, EblcTable::Builder* eblc,
                 const IntegerSet& glyph_ids) {
  EblcTableBuilderPtr eblc_builder = eblc;
  EbdtTableBuilderPtr ebdt_builder = ebdt;

  BitmapLocaList loca_list;
  BitmapSizeTableBuilderList* strikes = eblc_builder->BitmapSizeBuilders();

  // Note: Do not call eblc_builder->GenerateLocaList(&loca_list) and then
  //       ebdt_builder->SetLoca(loca_list).  For fonts like SimSun, there are
  //       >28K glyphs inside, where a typical usage will be <1K glyphs.  Doing
  //       the calls improperly will result in creation of >100K objects that
  //       will be destroyed immediately, inducing significant slowness.
  IntegerList removed_strikes;
  for (size_t i = 0; i < strikes->size(); i++) {
    if (!HasOverlap((*strikes)[i]->StartGlyphIndex(),
                    (*strikes)[i]->EndGlyphIndex(), glyph_ids)) {
      removed_strikes.push_back(i);
      continue;
    }

    IndexSubTableBuilderList* index_builders =
        (*strikes)[i]->IndexSubTableBuilders();
    IntegerList removed_indexes;
    BitmapGlyphInfoMap info_map;
    for (size_t j = 0; j < index_builders->size(); ++j) {
      int32_t first_glyph_id = (*index_builders)[j]->first_glyph_index();
      int32_t last_glyph_id = (*index_builders)[j]->last_glyph_index();
      if (!HasOverlap(first_glyph_id, last_glyph_id, glyph_ids)) {
        removed_indexes.push_back(j);
        continue;
      }
      for (IntegerSet::const_iterator gid = glyph_ids.begin(),
                                      gid_end = glyph_ids.end();
                                      gid != gid_end; gid++) {
        if (*gid < first_glyph_id) {
          continue;
        }
        if (*gid > last_glyph_id) {
          break;
        }
        BitmapGlyphInfoPtr info;
        info.Attach((*index_builders)[j]->GlyphInfo(*gid));
        if (info && info->length()) {  // Do not include gid without bitmap
          info_map[*gid] = info;
        }
      }
    }
    if (!info_map.empty()) {
      loca_list.push_back(info_map);
    } else {
      removed_strikes.push_back(i);  // Detected null entries.
    }

    // Remove unused index sub tables
    for (IntegerList::reverse_iterator j = removed_indexes.rbegin(),
                                       e = removed_indexes.rend();
                                       j != e; j++) {
      index_builders->erase(index_builders->begin() + *j);
    }
  }
  if (removed_strikes.size() == strikes->size() || loca_list.empty()) {
    return false;  // All strikes shall be gone.
  }

  // Remove unused strikes
  for (IntegerList::reverse_iterator j = removed_strikes.rbegin(),
                                     e = removed_strikes.rend(); j != e; j++) {
    strikes->erase(strikes->begin() + *j);
  }

  if (strikes->empty()) {  // no glyph covered, can safely drop the builders.
    return false;
  }

  ebdt_builder->SetLoca(&loca_list);
  ebdt_builder->GlyphBuilders();  // Initialize the builder.
  return true;
}

void CopyBigGlyphMetrics(BigGlyphMetrics::Builder* source,
                         BigGlyphMetrics::Builder* target) {
  target->SetHeight(static_cast<byte_t>(source->Height()));
  target->SetWidth(static_cast<byte_t>(source->Width()));
  target->SetHoriBearingX(static_cast<byte_t>(source->HoriBearingX()));
  target->SetHoriBearingY(static_cast<byte_t>(source->HoriBearingY()));
  target->SetHoriAdvance(static_cast<byte_t>(source->HoriAdvance()));
  target->SetVertBearingX(static_cast<byte_t>(source->VertBearingX()));
  target->SetVertBearingY(static_cast<byte_t>(source->VertBearingY()));
  target->SetVertAdvance(static_cast<byte_t>(source->VertAdvance()));
}

CALLER_ATTACH IndexSubTable::Builder*
ConstructIndexFormat4(IndexSubTable::Builder* b, const BitmapGlyphInfoMap& loca,
                      int32_t* image_data_offset) {
  IndexSubTableFormat4BuilderPtr builder4;
  builder4.Attach(IndexSubTableFormat4::Builder::CreateBuilder());
  CodeOffsetPairBuilderList offset_pairs;

  size_t offset = 0;
  int32_t lower_bound = b->first_glyph_index();
  int32_t upper_bound = b->last_glyph_index();
  bool lower_bound_reached = false;
  bool upper_bound_reached = false;
  int32_t last_gid = -1;
  BitmapGlyphInfoMap::const_iterator last_element = loca.end();
  --last_element;
  for (BitmapGlyphInfoMap::const_iterator i = loca.begin(), e = loca.end();
                                          i != e; i++) {
    int32_t gid = i->first;
    if (gid < lower_bound) {
      continue;
    }
    if (!lower_bound_reached) {
      builder4->set_first_glyph_index(gid);
      builder4->set_image_format(b->image_format());
      builder4->set_image_data_offset(*image_data_offset);
      last_gid = gid;
      lower_bound_reached = true;
    }
    if (gid > upper_bound) {
      upper_bound_reached = true;
    }
    if (!upper_bound_reached) {
      offset_pairs.push_back(
          IndexSubTableFormat4::CodeOffsetPairBuilder(gid, offset));
      offset += i->second->length();
      last_gid = gid;
      if (i == last_element) {
        upper_bound_reached = true;
      }
    }
    if (upper_bound_reached) {
      offset_pairs.push_back(
          IndexSubTableFormat4::CodeOffsetPairBuilder(-1, offset));
      builder4->set_last_glyph_index(last_gid);
      *image_data_offset += offset;
      break;
    }
  }
  builder4->SetOffsetArray(offset_pairs);

  return builder4.Detach();
}

CALLER_ATTACH IndexSubTable::Builder*
ConstructIndexFormat5(IndexSubTable::Builder* b, const BitmapGlyphInfoMap& loca,
                      int32_t* image_data_offset) {
  IndexSubTableFormat5BuilderPtr new_builder;
  new_builder.Attach(IndexSubTableFormat5::Builder::CreateBuilder());

  // Copy BigMetrics
  int32_t image_size = 0;
  if (b->index_format() == IndexSubTable::Format::FORMAT_2) {
    IndexSubTableFormat2BuilderPtr builder2 =
      down_cast<IndexSubTableFormat2::Builder*>(b);
    CopyBigGlyphMetrics(builder2->BigMetrics(), new_builder->BigMetrics());
    image_size = builder2->ImageSize();
  } else {
    IndexSubTableFormat5BuilderPtr builder5 =
      down_cast<IndexSubTableFormat5::Builder*>(b);
    BigGlyphMetricsBuilderPtr metrics_builder;
    CopyBigGlyphMetrics(builder5->BigMetrics(), new_builder->BigMetrics());
    image_size = builder5->ImageSize();
  }

  IntegerList* glyph_array = new_builder->GlyphArray();
  size_t offset = 0;
  int32_t lower_bound = b->first_glyph_index();
  int32_t upper_bound = b->last_glyph_index();
  bool lower_bound_reached = false;
  bool upper_bound_reached = false;
  int32_t last_gid = -1;
  BitmapGlyphInfoMap::const_iterator last_element = loca.end();
  --last_element;
  for (BitmapGlyphInfoMap::const_iterator i = loca.begin(), e = loca.end();
                                          i != e; i++) {
    int32_t gid = i->first;
    if (gid < lower_bound) {
      continue;
    }
    if (!lower_bound_reached) {
      new_builder->set_first_glyph_index(gid);
      new_builder->set_image_format(b->image_format());
      new_builder->set_image_data_offset(*image_data_offset);
      new_builder->SetImageSize(image_size);
      last_gid = gid;
      lower_bound_reached = true;
    }
    if (gid > upper_bound || i == last_element) {
      upper_bound_reached = true;
    }
    if (!upper_bound_reached || i == last_element) {
      glyph_array->push_back(gid);
      offset += i->second->length();
      last_gid = gid;
    }
    if (upper_bound_reached) {
      new_builder->set_last_glyph_index(last_gid);
      *image_data_offset += offset;
      break;
    }
  }
  return new_builder.Detach();
}

CALLER_ATTACH IndexSubTable::Builder*
SubsetIndexSubTable(IndexSubTable::Builder* builder,
                    const BitmapGlyphInfoMap& loca,
                    int32_t* image_data_offset) {
  switch (builder->index_format()) {
    case IndexSubTable::Format::FORMAT_1:
    case IndexSubTable::Format::FORMAT_3:
    case IndexSubTable::Format::FORMAT_4:
      return ConstructIndexFormat4(builder, loca, image_data_offset);
    case IndexSubTable::Format::FORMAT_2:
    case IndexSubTable::Format::FORMAT_5:
      return ConstructIndexFormat5(builder, loca, image_data_offset);
    default:
      assert(false);  // Shall not be here.
      break;
  }
  return NULL;
}

void SubsetEBLC(EblcTable::Builder* eblc, const BitmapLocaList& new_loca) {
  EblcTableBuilderPtr eblc_builder = eblc;
  BitmapSizeTableBuilderList* size_builders = eblc->BitmapSizeBuilders();
  if (size_builders == NULL) {
    return;  // No valid EBLC.
  }

  int32_t image_data_offset = EbdtTable::Offset::kHeaderLength;
  for (size_t strike = 0; strike < size_builders->size(); ++strike) {
    IndexSubTableBuilderList* index_builders =
        (*size_builders)[strike]->IndexSubTableBuilders();
    for (size_t index = 0; index < index_builders->size(); ++index) {
      IndexSubTable::Builder* new_builder_raw =
          SubsetIndexSubTable((*index_builders)[index], new_loca[strike],
                              &image_data_offset);
      if (NULL != new_builder_raw) {
        (*index_builders)[index].Attach(new_builder_raw);
      }
    }
  }
}

/******************************************************************************
  Long background comments

EBLC structure:
  header
  bitmapSizeTable[]
    one per strike
    holds strike metrics - sbitLineMetrics
    holds info about indexSubTableArray
  indexSubTableArray[][]
    one per strike and then one per indexSubTable for that strike
    holds info about the indexSubTable
    the indexSubTable entries pointed to can be of different formats
  indexSubTable
    one per indexSubTableArray entry
    tells how to get the glyphs
    may hold the glyph metrics if they are uniform for all the glyphs in range

There is nothing that says that the indexSubTableArray entries and/or the
indexSubTable items need to be unique. They may be shared between strikes.

EBDT structure:
  header
  glyphs
    amorphous blob of data
    different glyphs that are only able to be figured out from the EBLC table
    may hold metrics - depends on the EBLC entry that pointed to them

Subsetting EBLC table:
  Most pages use only a fraction (hundreds or less) glyphs out of a given font
  (which can have >20K glyphs for CJK).  It's safe to assume that the subset
  font will have sparse bitmap glyphs.  As a result, the EBLC table shall be
  reconstructed to either format 4 or 5.
*******************************************************************************/
bool SetupBitmapBuilders(Font* font, Font::Builder* builder,
                         const IntegerSet& glyph_ids, bool use_ebdt) {
  if (!font || !builder) {
    return false;
  }

  EbdtTablePtr ebdt_table =
      down_cast<EbdtTable*>(font->GetTable(use_ebdt ? Tag::EBDT : Tag::bdat));
  EblcTablePtr eblc_table =
      down_cast<EblcTable*>(font->GetTable(use_ebdt ? Tag::EBLC : Tag::bloc));

  // If the bitmap table's size is too small, skip subsetting.
  if (ebdt_table->DataLength() + eblc_table->DataLength() <
      BITMAP_SIZE_THRESHOLD) {
    return true;
  }

  // Get the builders.
  FontBuilderPtr font_builder = builder;
  EbdtTableBuilderPtr ebdt_table_builder = down_cast<EbdtTable::Builder*>(
      font_builder->NewTableBuilder(use_ebdt ? Tag::EBDT : Tag::bdat,
                                    ebdt_table->ReadFontData()));
  EblcTableBuilderPtr eblc_table_builder = down_cast<EblcTable::Builder*>(
      font_builder->NewTableBuilder(use_ebdt ? Tag::EBLC : Tag::bloc,
                                    eblc_table->ReadFontData()));
  if (ebdt_table_builder == NULL || eblc_table_builder == NULL) {
    // Out of memory.
    return false;
  }

  if (!ShallSubset(ebdt_table_builder, eblc_table_builder, glyph_ids)) {
    // Bitmap tables do not cover the glyphs in our subset.
    font_builder->RemoveTableBuilder(use_ebdt ? Tag::EBLC : Tag::bloc);
    font_builder->RemoveTableBuilder(use_ebdt ? Tag::EBDT : Tag::bdat);
    return false;
  }

  BitmapLocaList new_loca;
  ebdt_table_builder->GenerateLocaList(&new_loca);
  SubsetEBLC(eblc_table_builder, new_loca);

  return true;
}

enum BitmapDetection {
  kNotFound,
  kEBDTFound,
  kOnlyBDATFound
};

// Some fonts have both EBDT/EBLC and bdat/bloc, we need only one set of them.
int DetectBitmapBuilders(Font* font) {
  // Check if bitmap table exists.
  EbdtTablePtr ebdt_table = down_cast<EbdtTable*>(font->GetTable(Tag::EBDT));
  EblcTablePtr eblc_table = down_cast<EblcTable*>(font->GetTable(Tag::EBLC));
  if (ebdt_table == NULL && eblc_table == NULL) {
    // Check BDAT variants.
    ebdt_table = down_cast<EbdtTable*>(font->GetTable(Tag::bdat));
    eblc_table = down_cast<EblcTable*>(font->GetTable(Tag::bloc));
    if (ebdt_table == NULL || eblc_table == NULL) {
      // There's no bitmap tables.
      return kNotFound;
    }
    return kOnlyBDATFound;
  }
  return kEBDTFound;
}

SubsetterImpl::SubsetterImpl() {
}

SubsetterImpl::~SubsetterImpl() {
}

bool SubsetterImpl::LoadFont(const char* font_name,
                             const unsigned char* original_font,
                             size_t font_size) {
  MemoryInputStream mis;
  mis.Attach(original_font, font_size);
  if (factory_ == NULL) {
    factory_.Attach(FontFactory::GetInstance());
  }

  FontArray font_array;
  factory_->LoadFonts(&mis, &font_array);
  font_ = FindFont(font_name, font_array);
  if (font_ == NULL) {
    return false;
  }

  return true;
}

int SubsetterImpl::SubsetFont(const unsigned int* glyph_ids,
                              size_t glyph_count,
                              unsigned char** output_buffer) {
  if (factory_ == NULL || font_ == NULL) {
    return -1;
  }

  // Find glyf and loca table.
  GlyphTablePtr glyph_table =
      down_cast<GlyphTable*>(font_->GetTable(Tag::glyf));
  LocaTablePtr loca_table = down_cast<LocaTable*>(font_->GetTable(Tag::loca));
  if (glyph_table == NULL || loca_table == NULL) {
    // We are not able to subset the font.
    return 0;
  }

  IntegerSet glyph_id_processed;
  if (!ResolveCompositeGlyphs(glyph_table, loca_table,
                              glyph_ids, glyph_count, &glyph_id_processed) ||
      glyph_id_processed.empty()) {
    return 0;
  }

  FontPtr new_font;
  new_font.Attach(Subset(glyph_id_processed));
  if (new_font == NULL) {
    return 0;
  }

  MemoryOutputStream output_stream;
  factory_->SerializeFont(new_font, &output_stream);
  int length = static_cast<int>(output_stream.Size());
  if (length > 0) {
    *output_buffer = new unsigned char[length];
    memcpy(*output_buffer, output_stream.Get(), length);
  }

  return length;
}

/*******************************************************************************
  Long comments regarding TTF tables and PDF

According to PDF spec (section 9.9), the following tables must present:
  head, hhea, loca, maxp, cvt, prep, glyf, hmtx, fpgm
  cmap if font is used as a TTF and not a CIDFont dict

Other tables we need to keep for PDF rendering to support zoom in/out:
  bdat, bloc, ebdt, eblc, ebsc, gasp

Special table:
  CFF - if you have this table then you shouldn't have a glyf table and this is
        the table with all the glyphs.  Shall skip subsetting completely since
        sfntly is not capable of subsetting it for now.
  post - extra info here for printing on PostScript printers but maybe not
         enough to outweigh the space taken by the names

Tables to break apart:
  name - could throw away all but one language and one platform strings / might
         throw away some of the name entries
  cmap - could strip out non-needed cmap subtables
       - format 4 subtable can be subsetted as well using sfntly

Graphite tables:
  silf, glat, gloc, feat - shall be okay to strip out

Tables that can be discarded:
  OS/2 - everything here is for layout and description of the font that is
         elsewhere (some in the PDF objects)
  BASE, GDEF, GSUB, GPOS, JSTF - all used for layout
  kern - old style layout
  DSIG - this will be invalid after subsetting
  hdmx - layout
  PCLT - metadata that's not needed
  vmtx - layout
  vhea - layout
  VDMX
  VORG - not used by TT/OT - used by CFF
  hsty - would be surprised if you saw one of these - used on the Newton
  AAT tables - mort, morx, feat, acnt, bsin, just, lcar, fdsc, fmtx, prop,
               Zapf, opbd, trak, fvar, gvar, avar, cvar
             - these are all layout tables and once layout happens are not
               needed anymore
  LTSH - layout
*******************************************************************************/
CALLER_ATTACH Font* SubsetterImpl::Subset(const IntegerSet& glyph_ids) {
  // The const is initialized here to workaround VC bug of rendering all Tag::*
  // as 0.  These tags represents the TTF tables that we will embed in subset
  // font.
  const int32_t VALID_TABLE_TAG[] = {
    Tag::head, Tag::hhea, Tag::loca, Tag::maxp, Tag::cvt,
    Tag::prep, Tag::glyf, Tag::hmtx, Tag::fpgm, Tag::EBDT,
    Tag::EBLC, Tag::EBSC, Tag::bdat, Tag::bloc, Tag::bhed,
    Tag::cmap,  // Keep here for future tagged PDF development.
    Tag::name,  // Keep here due to legal concerns: copyright info inside.
  };

  // Setup font builders we need.
  FontBuilderPtr font_builder;
  font_builder.Attach(factory_->NewFontBuilder());
  IntegerSet remove_tags;

  GlyphTablePtr glyph_table =
      down_cast<GlyphTable*>(font_->GetTable(Tag::glyf));
  LocaTablePtr loca_table = down_cast<LocaTable*>(font_->GetTable(Tag::loca));

  if (SetupGlyfBuilders(font_builder, glyph_table, loca_table, glyph_ids)) {
    remove_tags.insert(Tag::glyf);
    remove_tags.insert(Tag::loca);
  }

  int flag = DetectBitmapBuilders(font_);
  if (flag != kNotFound) {
    bool use_ebdt = (flag == kEBDTFound);
    bool subset_success =
        SetupBitmapBuilders(font_, font_builder, glyph_ids, use_ebdt);

    if (use_ebdt || !subset_success) {
      remove_tags.insert(Tag::bdat);
      remove_tags.insert(Tag::bloc);
      remove_tags.insert(Tag::bhed);
    }
    if (use_ebdt && !subset_success) {
      remove_tags.insert(Tag::EBDT);
      remove_tags.insert(Tag::EBLC);
      remove_tags.insert(Tag::EBSC);
    }
  }

  IntegerSet allowed_tags;
  for (size_t i = 0; i < sizeof(VALID_TABLE_TAG) / sizeof(int32_t); ++i) {
    allowed_tags.insert(VALID_TABLE_TAG[i]);
  }
  for (IntegerSet::iterator i = remove_tags.begin(), e = remove_tags.end();
                            i != e; i++) {
    IntegerSet::iterator it = allowed_tags.find(*i);
    if (it != allowed_tags.end()) {
      allowed_tags.erase(it);
    }
  }

  // Setup remaining builders.
  for (IntegerSet::iterator i = allowed_tags.begin(), e = allowed_tags.end();
                            i != e; ++i) {
    Table* table = font_->GetTable(*i);
    if (table) {
      font_builder->NewTableBuilder(*i, table->ReadFontData());
    }
  }

  return font_builder->Build();
}

}  // namespace sfntly