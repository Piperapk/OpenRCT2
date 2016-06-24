#pragma region Copyright (c) 2014-2016 OpenRCT2 Developers
/*****************************************************************************
 * OpenRCT2, an open source clone of Roller Coaster Tycoon 2.
 *
 * OpenRCT2 is the work of many authors, a full list can be found in contributors.md
 * For more information, visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * A full copy of the GNU General Public License can be found in licence.txt
 *****************************************************************************/
#pragma endregion

#include "addresses.h"
#include "config.h"
#include "drawing/drawing.h"
#include "localisation/localisation.h"
#include "object.h"
#include "object_list.h"
#include "platform/platform.h"
#include "rct1.h"
#include "ride/ride.h"
#include "scenario.h"
#include "util/sawyercoding.h"
#include "world/entrance.h"
#include "world/footpath.h"
#include "world/scenery.h"
#include "world/water.h"

char gTempObjectLoadName[9] = { 0 };
uint32 gTotalNoImages = 0;

int object_load_entry(const utf8 *path, rct_object_entry *outEntry)
{
	SDL_RWops *file;

	file = SDL_RWFromFile(path, "rb");
	if (file == NULL)
		return 0;

	if (SDL_RWread(file, outEntry, sizeof(rct_object_entry), 1) != 1) {
		SDL_RWclose(file);
		return 0;
	}

	SDL_RWclose(file);
	return 1;
}

int object_load_file(int groupIndex, const rct_object_entry *entry, int* chunkSize, const rct_object_entry *installedObject)
{
	uint8 objectType;
	rct_object_entry openedEntry;
	char path[MAX_PATH];
	SDL_RWops* rw;

	substitute_path(path, gRCT2AddressObjectDataPath, (char*)installedObject + 16);

	log_verbose("loading object, %s", path);

	rw = SDL_RWFromFile(path, "rb");
	if (rw == NULL)
		return 0;

	SDL_RWread(rw, &openedEntry, sizeof(rct_object_entry), 1);
	if (!object_entry_compare(&openedEntry, entry)) {
		SDL_RWclose(rw);
		return 0;
	}

	// Get chunk size
	uint8 *installedObject_pointer = (uint8*)installedObject + 16;
	// Skip file name
	while (*installedObject_pointer++);

	// Read chunk size
	*chunkSize = *((uint32*)installedObject_pointer);
	uint8 *chunk;

	if (*chunkSize == 0xFFFFFFFF) {
		chunk = (uint8*)malloc(0x600000);
		assert(chunk != NULL);
		*chunkSize = sawyercoding_read_chunk_with_size(rw, chunk, 0x600000);
		chunk = realloc(chunk, *chunkSize);
	}
	else {
		chunk = (uint8*)malloc(*chunkSize);
		*chunkSize = sawyercoding_read_chunk_with_size(rw, chunk, *chunkSize);
	}
	SDL_RWclose(rw);
	if (chunk == NULL) {
		log_error("Failed to load object from %s of size %d", path, *chunkSize);
		return 0;
	}

	int calculatedChecksum = object_calculate_checksum(&openedEntry, chunk, *chunkSize);

	// Calculate and check checksum
	if (calculatedChecksum != openedEntry.checksum && !gConfigGeneral.allow_loading_with_incorrect_checksum) {
		log_error("Object Load failed due to checksum failure: calculated checksum %d, object says %d.", calculatedChecksum, (int)openedEntry.checksum);
		free(chunk);
		return 0;
			
	}

	objectType = openedEntry.flags & 0x0F;

	if (!object_test(objectType, chunk)) {
		log_error("Object Load failed due to paint failure.");
		free(chunk);
		return 0;
	}

	if (gTotalNoImages >= 0x4726E){
		log_error("Object Load failed due to too many images loaded.");
		free(chunk);
		return 0;
	}

	void** chunk_list = object_entry_groups[objectType].chunks;
	if (groupIndex == -1) {
		for (groupIndex = 0; chunk_list[groupIndex] != (void*)-1; groupIndex++) {
			if (groupIndex + 1 >= object_entry_group_counts[objectType]) {
				log_error("Object Load failed due to too many objects of a certain type.");
				free(chunk);
				return 0;
			}
		}
	}

	if (RCT2_GLOBAL(0x9ADAFD, uint8) != 0) {
		chunk = object_load(objectType, chunk, groupIndex, chunkSize);
	}

	chunk_list[groupIndex] = chunk;

	rct_object_entry_extended* extended_entry = &object_entry_groups[objectType].entries[groupIndex];

	memcpy(extended_entry, &openedEntry, sizeof(rct_object_entry));
	extended_entry->chunk_size = *chunkSize;

	gLastLoadedObjectChunkData = chunk;

	return 1;
}

/**
 *
 *  rct2: 0x006A985D
 */
int object_load_chunk(int groupIndex, rct_object_entry *entry, int* chunkSize)
{
	// Alow chunkSize to be null
	int tempChunkSize;
	if (chunkSize == NULL)
		chunkSize = &tempChunkSize;

	RCT2_GLOBAL(0xF42B64, uint32) = groupIndex;

	if (gInstalledObjectsCount == 0) {
		log_error("Object Load failed due to no items installed check.");
		return 1;
	}

	rct_object_entry *installedObject = object_list_find(entry);
	if (installedObject == NULL) {
		log_error("object not installed");
		return 0;
	}

	if (object_load_file(groupIndex, entry, chunkSize, installedObject))
		return 1;

	return 0;
}

/**
 *
 *  rct2: 0x006a9f42
 *  ebx: file
 *  ebp: entry
 */
int write_object_file(SDL_RWops *rw, rct_object_entry* entry)
{
	uint8 entryGroupIndex = 0, type = 0;
	uint8* chunk = 0;

	if (!find_object_in_entry_group(entry, &type, &entryGroupIndex))return 0;

	chunk = object_entry_groups[type].chunks[entryGroupIndex];

	object_unload(type, chunk);

	rct_object_entry_extended* installed_entry = &object_entry_groups[type].entries[entryGroupIndex];
	uint8* dst_buffer = (uint8*)malloc(0x600000);
	memcpy(dst_buffer, installed_entry, sizeof(rct_object_entry));

	uint32 size_dst = sizeof(rct_object_entry);

	sawyercoding_chunk_header chunkHeader;
	// Encoding type (not used anymore)
	RCT2_GLOBAL(0x9E3CBD, uint8) = object_entry_group_encoding[type];

	chunkHeader.encoding = object_entry_group_encoding[type];
	chunkHeader.length = installed_entry->chunk_size;


	//Check if content of object file matches the stored checksum. If it does not, then fix it.
	int calculated_checksum = object_calculate_checksum(entry, chunk, installed_entry->chunk_size);
	if(entry->checksum != calculated_checksum) {
		//Store the current length of the header - it's the offset at which we will write the extra bytes
		int salt_offset = chunkHeader.length;
		/*Allocate a new chunk 11 bytes longer.
		I would just realloc the old one, but realloc can move the data, leaving dangling pointers 
		into the old buffer. If the chunk is only referenced in one place it would be safe to realloc 
		it and update that reference, but I don't know the codebase well enough to know if that's the
		case, so to be on the safe side I copy it*/
		uint8* new_chunk = malloc(chunkHeader.length + 11);
		memcpy(new_chunk,chunk, chunkHeader.length);
		//It should be safe to update these in-place because they are local
		chunkHeader.length += 11;

		/*Next work out which bits need to be flipped to make the current checksum match the one in the file 
		The bitwise rotation compensates for the rotation performed during the checksum calculation*/
		int bits_to_flip = entry->checksum ^ ((calculated_checksum << 25) | (calculated_checksum >> 7));
		/*Each set bit encountered during encoding flips one bit of the resulting checksum (so each bit of the checksum is an XOR
		of bits from the file). Here, we take each bit that should be flipped in the checksum and set one of the bits in the data 
		that maps to it. 11 bytes is the minimum needed to touch every bit of the checksum - with less than that, you wouldn't 
		always be able to make the checksum come out to the desired target*/
		new_chunk[salt_offset] = (bits_to_flip & 0x00000001) << 7;
		new_chunk[salt_offset + 1] = ((bits_to_flip & 0x00200000) >> 14);
		new_chunk[salt_offset + 2] = ((bits_to_flip & 0x000007F8) >> 3);
		new_chunk[salt_offset + 3] = ((bits_to_flip & 0xFF000000) >> 24);
		new_chunk[salt_offset + 4] = ((bits_to_flip & 0x00100000) >> 13);
		new_chunk[salt_offset + 5] = (bits_to_flip & 0x00000004) >> 2;
		new_chunk[salt_offset + 6] = 0;
		new_chunk[salt_offset + 7] = ((bits_to_flip & 0x000FF000) >> 12);
		new_chunk[salt_offset + 8] = (bits_to_flip & 0x00000002) >> 1;
		new_chunk[salt_offset + 9] = (bits_to_flip & 0x00C00000) >> 22;
		new_chunk[salt_offset + 10] = (bits_to_flip & 0x00000800) >> 11;

		//Write modified chunk data
		size_dst += sawyercoding_write_chunk_buffer(dst_buffer + sizeof(rct_object_entry),new_chunk,chunkHeader);
		free(new_chunk);
	} else {
		//If the checksum matches, write chunk data
		size_dst += sawyercoding_write_chunk_buffer(dst_buffer + sizeof(rct_object_entry), chunk, chunkHeader);
	}

	SDL_RWwrite(rw, dst_buffer, 1, size_dst);
	free(dst_buffer);
	return 1;
}

/**
*
*  rct2: 0x006AA2B7
*/
int object_load_packed(SDL_RWops* rw)
{
	object_unload_all();

	rct_object_entry entry;

	SDL_RWread(rw, &entry, 16, 1);

	uint8* chunk = (uint8*)malloc(0x600000);
	uint32 chunkSize = sawyercoding_read_chunk(rw, chunk);
	chunk = realloc(chunk, chunkSize);

	if (chunk == NULL){
		log_error("Failed to allocate memory for packed object.");
		return 0;
	}

	if (object_calculate_checksum(&entry, chunk, chunkSize) != entry.checksum){
	
		if(gConfigGeneral.allow_loading_with_incorrect_checksum) {
			log_warning("Checksum mismatch from packed object: %.8s", entry.name);
		} else {
			log_error("Checksum mismatch from packed object: %.8s", entry.name);
			free(chunk);
			return 0;
		}
	}

	int type = entry.flags & 0x0F;

	if (!object_test(type, chunk)) {
		log_error("Packed object failed paint test.");
		free(chunk);
		return 0;
	}

	if (gTotalNoImages >= 0x4726E){
		log_error("Packed object has too many images.");
		free(chunk);
		return 0;
	}


	int entryGroupIndex = 0;

	for (; entryGroupIndex < object_entry_group_counts[type]; entryGroupIndex++){
		if (object_entry_groups[type].chunks[entryGroupIndex] == (uint8*)-1){
			break;
		}
	}

	if (entryGroupIndex == object_entry_group_counts[type]){
		// This should never occur. Objects are not loaded before installing a
		// packed object. So there is only one object loaded at this point.
		log_error("Too many objects of the same type loaded.");
		free(chunk);
		return 0;
	}

	// Copy the entry into the relevant entry group.
	object_entry_groups[type].chunks[entryGroupIndex] = chunk;
	rct_object_entry_extended* extended_entry = &object_entry_groups[type].entries[entryGroupIndex];
	memcpy(extended_entry, &entry, sizeof(rct_object_entry));
	extended_entry->chunk_size = chunkSize;

	// Ensure the entry does not already exist.
	rct_object_entry *installedObject = gInstalledObjects;
	if (gInstalledObjectsCount){
		for (uint32 i = 0; i < gInstalledObjectsCount; ++i){
			if (object_entry_compare(&entry, installedObject)){
				object_unload_all();
				return 0;
			}
			installedObject = object_get_next(installedObject);
		}
	}

	// Convert the entry name to a upper case path name
	char path[MAX_PATH];
	char objectPath[9] = { 0 };
	for (int i = 0; i < 8; ++i){
		if (entry.name[i] != ' ')
			objectPath[i] = toupper(entry.name[i]);
		else
			objectPath[i] = '\0';
	}

	substitute_path(path, gRCT2AddressObjectDataPath, objectPath);
	// Require pointer to start of filename
	char* last_char = path + strlen(path);
	strcat(path, ".DAT");

	// Check that file does not exist
	// Adjust filename if it does.
	for (; platform_file_exists(path);){
		for (char* curr_char = last_char - 1;; --curr_char){
			if (*curr_char == '\\'){
				substitute_path(path, gRCT2AddressObjectDataPath, "00000000.DAT");
				break;
			}
			if (*curr_char < '0') *curr_char = '0';
			else if (*curr_char == '9') *curr_char = 'A';
			else if (*curr_char == 'Z') *curr_char = '0';
			else (*curr_char)++;
			if (*curr_char != '0') break;
		}
	}

	// Actually write the object to the file
	SDL_RWops* rw_out = SDL_RWFromFile(path, "wb");
	if (rw_out != NULL){
		uint8 result = write_object_file(rw_out, &entry);

		SDL_RWclose(rw_out);
		object_unload_all();

		return result;
	}

	object_unload_all();
	return 0;
}

/**
 *
 *  rct2: 0x006A9CAF
 */
void object_unload_chunk(rct_object_entry *entry)
{
	uint8 object_type, object_index;
	if (!find_object_in_entry_group(entry, &object_type, &object_index)){
		return;
	}

	uint8* chunk = object_entry_groups[object_type].chunks[object_index];

	object_unload(object_type, chunk);

	free(chunk);
	memset(&object_entry_groups[object_type].entries[object_index], 0, sizeof(rct_object_entry_extended));
	object_entry_groups[object_type].chunks[object_index] = (uint8*)-1;
}

int object_entry_compare(const rct_object_entry *a, const rct_object_entry *b)
{
	// If an official object don't bother checking checksum
	if ((a->flags & 0xF0) || (b->flags & 0xF0)) {
		if ((a->flags & 0x0F) != (b->flags & 0x0F))
			return 0;
		int match = memcmp(a->name, b->name, 8);
		if (match)
			return 0;
	}
	else {
		if (a->flags != b->flags)
			return 0;
		int match = memcmp(a->name, b->name, 8);
		if (match)
			return 0;
		if (a->checksum != b->checksum)
			return 0;
	}

	return 1;
}

int object_calculate_checksum(const rct_object_entry *entry, const uint8 *data, int dataLength)
{
	const uint8 *entryBytePtr = (uint8*)entry;

	uint32 checksum = 0xF369A75B;
	checksum ^= entryBytePtr[0];
	checksum = rol32(checksum, 11);
	for (int i = 4; i < 12; i++) {
		checksum ^= entryBytePtr[i];
		checksum = rol32(checksum, 11);
	}
	for (int i = 0; i < dataLength; i++) {
		checksum ^= data[i];
		checksum = rol32(checksum, 11);
	}
	return (int)checksum;
}

/**
 *
 *  rct2: 0x006A9ED1
 */
int object_chunk_load_image_directory(uint8_t** chunk)
{
	int image_start_no = gTotalNoImages;

	// First dword of chunk is no_images
	int no_images = *((uint32_t*)(*chunk));
	*chunk += 4;
	// Second dword of chunk is length of image data
	int length_of_data = *((uint32_t*)(*chunk));
	*chunk += 4;

	gTotalNoImages = no_images + image_start_no;

	rct_g1_element* g1_dest = &g1Elements[image_start_no];

	// After length of data is the start of all g1 element structs
	rct_g1_element_32bit* g1_source = (rct_g1_element_32bit*)(*chunk);

	// After the g1 element structs is the actual images.
	uintptr_t image_offset = no_images * sizeof(rct_g1_element_32bit) + (uintptr_t)g1_source;

	for (int i = 0; i < no_images; ++i) {
		g1_dest->offset        = (uint8*)(g1_source->offset + image_offset);
		g1_dest->width         = g1_source->width;
		g1_dest->height        = g1_source->height;
		g1_dest->x_offset      = g1_source->x_offset;
		g1_dest->y_offset      = g1_source->y_offset;
		g1_dest->flags         = g1_source->flags;
		g1_dest->zoomed_offset = g1_source->zoomed_offset;
		g1_dest++;

		drawing_engine_invalidate_image(image_start_no + i);
		g1_source++;
	}

	*chunk = ((uint8*)g1_source) + length_of_data;

	return image_start_no;
}

/**
 * object_load_func will receive the struct as it is stored in a file, this
 * means 32bit pointers. On non-32bit platforms it's supposed to upconvert
 * structure to native pointer types and return pointer to newly allocated
 * space. It should not be the same pointer as input.
 *
 * object_unload_func will receive a pointer to the structure created with
 * object_load_func, i.e. with native pointer types
 *
 * object_test_func will receive a pointer to the struct as it is stored in a
 * file.
 *
 * object_paint_func and object_paint_func will receive structs with native
 * pointer types.
 */
typedef uint8* (*object_load_func)(void *objectEntry, uint32 entryIndex, int *chunkSize);
typedef void   (*object_unload_func)(void *objectEntry);
typedef bool   (*object_test_func)(void *objectEntry);
typedef void   (*object_paint_func)(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y);
typedef rct_string_id (*object_desc_func)(void *objectEntry);
typedef void   (*object_reset_func)(void *objectEntry, uint32 entryIndex);

/**
 * Represents addresses for virtual object functions.
 */
typedef struct object_type_vtable {
	object_load_func   load;
	object_unload_func unload;
	object_test_func   test;
	object_paint_func  paint;
	object_desc_func   desc;
	object_reset_func  reset;
} object_type_vtable;

///////////////////////////////////////////////////////////////////////////////
// Ride (rct2: 0x006E6E2A)
///////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 1)
/**
 * Ride type vehicle structure.
 * size: 0x65
 */
typedef struct rct_ride_entry_vehicle_32bit {
	uint16 rotation_frame_mask;		// 0x00 , 0x1A
	uint8 var_02;					// 0x02 , 0x1C
	uint8 var_03;					// 0x03 , 0x1D
	uint32 spacing;					// 0x04 , 0x1E
	uint16 car_friction;			// 0x08 , 0x22
	sint8 tab_height;				// 0x0A , 0x24
	uint8 num_seats;				// 0x0B , 0x25
	uint16 sprite_flags;			// 0x0C , 0x26
	uint8 sprite_width;				// 0x0E , 0x28
	uint8 sprite_height_negative;	// 0x0F , 0x29
	uint8 sprite_height_positive;	// 0x10 , 0x2A
	uint8 var_11;					// 0x11 , 0x2B
	uint16 flags_a;					// 0x12 , 0x2C
	uint16 flags_b;					// 0x14 , 0x2E
	uint16 var_16;					// 0x16 , 0x30
	uint32 base_image_id;			// 0x18 , 0x32
	uint32 var_1C;					// 0x1C , 0x36
	uint32 var_20;					// 0x20 , 0x3A
	uint32 var_24;					// 0x24 , 0x3E
	uint32 var_28;					// 0x28 , 0x42
	uint32 var_2C;					// 0x2C , 0x46
	uint32 var_30;					// 0x30 , 0x4A
	uint32 var_34;					// 0x34 , 0x4E
	uint32 var_38;					// 0x38 , 0x52
	uint32 var_3C;					// 0x3C , 0x56
	uint32 var_40;					// 0x40 , 0x5A
	uint32 var_44;					// 0x44 , 0x5E
	uint32 var_48;					// 0x48 , 0x62
	uint32 var_4C;					// 0x4C , 0x66
	uint32 no_vehicle_images;		// 0x50 , 0x6A
	uint8 no_seating_rows;			// 0x54 , 0x6E
	uint8 spinning_inertia;			// 0x55 , 0x6F
	uint8 spinning_friction;		// 0x56 , 0x70
	uint8 friction_sound_id;		// 0x57 , 0x71
	uint8 var_58;					// 0x58 , 0x72
	uint8 sound_range;				// 0x59 , 0x73
	uint8 var_5A;					// 0x5A , 0x74
	uint8 powered_acceleration;		// 0x5B , 0x75
	uint8 powered_max_speed;		// 0x5C , 0x76
	uint8 car_visual;				// 0x5D , 0x77
	uint8 effect_visual;
	uint8 draw_order;
	uint8 special_frames;			// 0x60 , 0x7A
	uint32 peep_loading_positions;	// 0x61 , 0x7B note: uint32
} rct_ride_entry_vehicle_32bit;
assert_struct_size(rct_ride_entry_vehicle_32bit, 0x65);

/**
 * Ride type structure.
 * size: unknown
 */
typedef struct rct_ride_entry_32bit {
	rct_string_id name;						// 0x000
	rct_string_id description;				// 0x002
	uint32 images_offset;					// 0x004
	uint32 flags;							// 0x008
	uint8 ride_type[3];						// 0x00C
	uint8 min_cars_in_train;				// 0x00F
	uint8 max_cars_in_train;				// 0x010
	uint8 cars_per_flat_ride;				// 0x011
	// Number of cars that can't hold passengers
	uint8 zero_cars;						// 0x012
	// The index to the vehicle type displayed in
	// the vehicle tab.
	uint8 tab_vehicle;						// 0x013
	uint8 default_vehicle;					// 0x014
	// Convert from first - fourth vehicle to
	// vehicle structure
	uint8 front_vehicle;					// 0x015
	uint8 second_vehicle;					// 0x016
	uint8 rear_vehicle;						// 0x017
	uint8 third_vehicle;					// 0x018
	uint8 pad_019;
	rct_ride_entry_vehicle_32bit vehicles[4];		// 0x1A note: 32bit!
	uint32 vehicle_preset_list;				// 0x1AE note: uint32!
	sint8 excitement_multipler;				// 0x1B2
	sint8 intensity_multipler;				// 0x1B3
	sint8 nausea_multipler;					// 0x1B4
	uint8 max_height;						// 0x1B5
	union {
		uint64 enabledTrackPieces;						// 0x1B6
		struct {
			uint32 enabledTrackPiecesA;					// 0x1B6
			uint32 enabledTrackPiecesB;					// 0x1BA
		};
	};
	uint8 category[2];									// 0x1BE
	uint8 shop_item;									// 0x1C0
	uint8 shop_item_secondary;							// 0x1C1
} rct_ride_entry_32bit;
assert_struct_size(rct_ride_entry_32bit, 0x1c2);
#pragma pack(pop)

static uint8* object_type_ride_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_ride_entry_32bit *rideEntry = (rct_ride_entry_32bit*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_ride_entry_32bit));
	const size_t extendedDataSize = *chunkSize - sizeof(rct_ride_entry_32bit);
	*chunkSize = *chunkSize + sizeof(rct_ride_entry) - sizeof(rct_ride_entry_32bit);
	assert(*chunkSize > 0);
	rct_ride_entry* outRideEntry = malloc(*chunkSize);
	assert(outRideEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outRideEntry + sizeof(rct_ride_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	// After rideEntry is 3 string tables
	rideEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_RIDE, entryIndex, 0);
	rideEntry->description = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_RIDE, entryIndex, 1);

	//TODO: Move to its own function when ride construction window is merged.
	if (gConfigInterface.select_by_track_type) {
		rideEntry->enabledTrackPieces = 0xFFFFFFFFFFFFFFFF;
	}

	object_get_localised_text(&extendedEntryData, OBJECT_TYPE_RIDE, entryIndex, 2);
	outRideEntry->vehicle_preset_list = (vehicle_colour_preset_list*)extendedEntryData;

	// If Unknown struct size is 0xFF then there are 32 3 byte structures
	uint8 unknown_size = *extendedEntryData++;
	if (unknown_size != 0xFF) {
		extendedEntryData += unknown_size * 3;
	} else {
		extendedEntryData += 0x60;
	}

	sint8 *peep_loading_positions = (sint8*)extendedEntryData;
	// Peep loading positions variable size
	// 4 different vehicle subtypes are available
	for (int i = 0; i < 4; i++){
		uint16 no_peep_positions = *extendedEntryData++;
		// If no_peep_positions is 0xFF then no_peep_positions is a word
		if (no_peep_positions == 0xFF) {
			no_peep_positions = *((uint16*)extendedEntryData);
			extendedEntryData += 2;
		}
		extendedEntryData += no_peep_positions;
	}

	int images_offset = object_chunk_load_image_directory(&extendedEntryData);
	outRideEntry->images_offset = images_offset;

	int cur_vehicle_images_offset = images_offset + 3;

	for (int i = 0; i < 4; i++) {
		rct_ride_entry_vehicle_32bit* vehicleEntry = &rideEntry->vehicles[i];
		rct_ride_entry_vehicle* outVehicleEntry = &outRideEntry->vehicles[i];

		if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT) {
			int al = 1;
			if (vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_SWINGING) {
				al = 13;
				if ((vehicleEntry->flags_b & (VEHICLE_ENTRY_FLAG_B_5 | VEHICLE_ENTRY_FLAG_B_11)) != (VEHICLE_ENTRY_FLAG_B_5 | VEHICLE_ENTRY_FLAG_B_11)) {
					al = 7;
					if (!(vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_5)) {
						if (!(vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_11)) {
							al = 5;
							if (vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_9) {
								al = 3;
							}
						}
					}
				}
			}
			vehicleEntry->var_03 = al;
			// 0x6DE90B

			al = 0x20;
			if (!(vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_14)) {
				al = 1;
				if (vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_7) {
					if (vehicleEntry->var_11 != 6) {
						al = 2;
						if (!(vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_7)) {
							al = 4;
						}
					}
				}
			}
			if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_12) {
				al = vehicleEntry->special_frames;
			}
			vehicleEntry->var_02 = al;
			// 0x6DE946

			vehicleEntry->var_16 = vehicleEntry->var_02 * vehicleEntry->var_03;
			vehicleEntry->base_image_id = cur_vehicle_images_offset;
			int image_index = vehicleEntry->base_image_id;

			if (vehicleEntry->car_visual != VEHICLE_VISUAL_RIVER_RAPIDS) {
				int b = vehicleEntry->var_16 * 32;

				if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_11) b /= 2;
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_15) b /= 8;

				image_index += b;

				// Incline 25
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_GENTLE_SLOPES) {
					vehicleEntry->var_20 = image_index;
					b = vehicleEntry->var_16 * 72;
					if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_14)
						b = vehicleEntry->var_16 * 16;

					image_index += b;
				}

				// Incline 60
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_STEEP_SLOPES) {
					vehicleEntry->var_24 = image_index;
					b = vehicleEntry->var_16 * 80;
					image_index += b;
				}

				// Verticle
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_VERTICAL_SLOPES) {
					vehicleEntry->var_28 = image_index;
					b = vehicleEntry->var_16 * 116;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_DIAGONAL_SLOPES) {
					vehicleEntry->var_2C = image_index;
					b = vehicleEntry->var_16 * 24;
					image_index += b;
				}

				// Bank
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT_BANKED) {
					vehicleEntry->var_30 = image_index;
					b = vehicleEntry->var_16 * 80;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_INLINE_TWISTS) {
					vehicleEntry->var_34 = image_index;
					b = vehicleEntry->var_16 * 40;
					image_index += b;
				}

				// Track half? Up/Down
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT_TO_GENTLE_SLOPE_BANKED_TRANSITIONS) {
					vehicleEntry->var_38 = image_index;
					b = vehicleEntry->var_16 * 128;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_DIAGONAL_GENTLE_SLOPE_BANKED_TRANSITIONS) {
					vehicleEntry->var_3C = image_index;
					b = vehicleEntry->var_16 * 16;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_GENTLE_SLOPE_BANKED_TRANSITIONS) {
					vehicleEntry->var_40 = image_index;
					b = vehicleEntry->var_16 * 16;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_GENTLE_SLOPE_BANKED_TURNS) {
					vehicleEntry->var_44 = image_index;
					b = vehicleEntry->var_16 * 128;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT_TO_GENTLE_SLOPE_WHILE_BANKED_TRANSITIONS) {
					vehicleEntry->var_48 = image_index;
					b = vehicleEntry->var_16 * 16;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_CORKSCREWS) {
					vehicleEntry->var_4C = image_index;
					b = vehicleEntry->var_16 * 80;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_RESTRAINT_ANIMATION) {
					vehicleEntry->var_1C = image_index;
					b = vehicleEntry->var_16 * 12;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_14) {
					// Same offset as above???
					vehicleEntry->var_4C = image_index;
					b = vehicleEntry->var_16 * 32;
					image_index += b;
				}
			} else {
				image_index += vehicleEntry->var_16 * 36;
			}
			// No vehicle images
			vehicleEntry->no_vehicle_images = image_index - cur_vehicle_images_offset;

			// Move the offset over this vehicles images. Including peeps
			cur_vehicle_images_offset = image_index + vehicleEntry->no_seating_rows * vehicleEntry->no_vehicle_images;
			// 0x6DEB0D

			// Copy the vehicle entry over to new one
			outVehicleEntry->rotation_frame_mask = vehicleEntry->rotation_frame_mask;
			outVehicleEntry->var_02 = vehicleEntry->var_02;
			outVehicleEntry->var_03 = vehicleEntry->var_03;
			outVehicleEntry->spacing = vehicleEntry->spacing;
			outVehicleEntry->car_friction = vehicleEntry->car_friction;
			outVehicleEntry->tab_height = vehicleEntry->tab_height;
			outVehicleEntry->num_seats = vehicleEntry->num_seats;
			outVehicleEntry->sprite_flags = vehicleEntry->sprite_flags;
			outVehicleEntry->sprite_width = vehicleEntry->sprite_width;
			outVehicleEntry->sprite_height_negative = vehicleEntry->sprite_height_negative;
			outVehicleEntry->sprite_height_positive = vehicleEntry->sprite_height_positive;
			outVehicleEntry->var_11 = vehicleEntry->var_11;
			outVehicleEntry->flags_a = vehicleEntry->flags_a;
			outVehicleEntry->flags_b = vehicleEntry->flags_b;
			outVehicleEntry->var_16 = vehicleEntry->var_16;
			outVehicleEntry->base_image_id = vehicleEntry->base_image_id;
			outVehicleEntry->var_1C = vehicleEntry->var_1C;
			outVehicleEntry->var_20 = vehicleEntry->var_20;
			outVehicleEntry->var_24 = vehicleEntry->var_24;
			outVehicleEntry->var_28 = vehicleEntry->var_28;
			outVehicleEntry->var_2C = vehicleEntry->var_2C;
			outVehicleEntry->var_30 = vehicleEntry->var_30;
			outVehicleEntry->var_34 = vehicleEntry->var_34;
			outVehicleEntry->var_38 = vehicleEntry->var_38;
			outVehicleEntry->var_3C = vehicleEntry->var_3C;
			outVehicleEntry->var_40 = vehicleEntry->var_40;
			outVehicleEntry->var_44 = vehicleEntry->var_44;
			outVehicleEntry->var_48 = vehicleEntry->var_48;
			outVehicleEntry->var_4C = vehicleEntry->var_4C;
			outVehicleEntry->no_vehicle_images = vehicleEntry->no_vehicle_images;
			outVehicleEntry->no_seating_rows = vehicleEntry->no_seating_rows;
			outVehicleEntry->spinning_inertia = vehicleEntry->spinning_inertia;
			outVehicleEntry->spinning_friction = vehicleEntry->spinning_friction;
			outVehicleEntry->friction_sound_id = vehicleEntry->friction_sound_id;
			outVehicleEntry->var_58 = vehicleEntry->var_58;
			outVehicleEntry->sound_range = vehicleEntry->sound_range;
			outVehicleEntry->var_5A = vehicleEntry->var_5A;
			outVehicleEntry->powered_acceleration = vehicleEntry->powered_acceleration;
			outVehicleEntry->powered_max_speed = vehicleEntry->powered_max_speed;
			outVehicleEntry->car_visual = vehicleEntry->car_visual;
			outVehicleEntry->effect_visual = vehicleEntry->effect_visual;
			outVehicleEntry->draw_order = vehicleEntry->draw_order;
			outVehicleEntry->special_frames = vehicleEntry->special_frames;

			if (!(vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_10)) {
				int num_images = cur_vehicle_images_offset - vehicleEntry->base_image_id;
				if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_13) {
					num_images *= 2;
				}

				set_vehicle_type_image_max_sizes(outVehicleEntry, num_images);
			}

			sint8 no_positions = *peep_loading_positions++;
			if (no_positions == -1) {
				// The no_positions is 16 bit skip over
				peep_loading_positions += 2;
			}
			// not set for original entry
			outVehicleEntry->peep_loading_positions = peep_loading_positions;
		}
	}

	// 0x6DEB71
	if (RCT2_GLOBAL(0x9ADAFD, uint8) == 0) {
		for (int i = 0; i < 3; i++) {
			int dl = rideEntry->ride_type[i];
			if (dl == 0xFF) {
				continue;
			}

			uint8 *typeToRideEntryIndexMap = gTypeToRideEntryIndexMap;
			while (dl >= 0) {
				if (*typeToRideEntryIndexMap++ == 0xFF) {
					dl--;
				}
			}

			typeToRideEntryIndexMap--;
			uint8 previous_entry = entryIndex;
			while (typeToRideEntryIndexMap < gTypeToRideEntryIndexMap + countof(gTypeToRideEntryIndexMap)){
				uint8 backup_entry = *typeToRideEntryIndexMap;
				*typeToRideEntryIndexMap++ = previous_entry;
				previous_entry = backup_entry;
			}
		}
	}

	// 0x6DEBAA
	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	int di = rideEntry->ride_type[0] | (rideEntry->ride_type[1] << 8) | (rideEntry->ride_type[2] << 16);

	outRideEntry->name = rideEntry->name;
	outRideEntry->description = rideEntry->description;
	outRideEntry->images_offset = rideEntry->images_offset;
	outRideEntry->flags = rideEntry->flags;
	outRideEntry->ride_type[0] = rideEntry->ride_type[0];
	outRideEntry->ride_type[1] = rideEntry->ride_type[1];
	outRideEntry->ride_type[2] = rideEntry->ride_type[2];
	outRideEntry->min_cars_in_train = rideEntry->min_cars_in_train;
	outRideEntry->max_cars_in_train = rideEntry->max_cars_in_train;
	outRideEntry->cars_per_flat_ride = rideEntry->cars_per_flat_ride;
	outRideEntry->zero_cars = rideEntry->zero_cars;
	outRideEntry->tab_vehicle = rideEntry->tab_vehicle;
	outRideEntry->default_vehicle = rideEntry->default_vehicle;
	outRideEntry->front_vehicle = rideEntry->front_vehicle;
	outRideEntry->second_vehicle = rideEntry->second_vehicle;
	outRideEntry->rear_vehicle = rideEntry->rear_vehicle;
	outRideEntry->third_vehicle = rideEntry->third_vehicle;
	outRideEntry->pad_019 = rideEntry->pad_019;
	// 0x1a vehicles already set
	// 0a1ae vehicle_preset_list already set
	outRideEntry->excitement_multipler = rideEntry->excitement_multipler;
	outRideEntry->intensity_multipler = rideEntry->intensity_multipler;
	outRideEntry->nausea_multipler = rideEntry->nausea_multipler;
	outRideEntry->max_height = rideEntry->max_height;
	outRideEntry->enabledTrackPieces = rideEntry->enabledTrackPieces;
	outRideEntry->category[0] = rideEntry->category[0];
	outRideEntry->category[1] = rideEntry->category[1];
	outRideEntry->shop_item = rideEntry->shop_item;
	outRideEntry->shop_item_secondary = rideEntry->shop_item_secondary;

	if ((rideEntry->flags & RIDE_ENTRY_FLAG_SEPARATE_RIDE_NAME) && !rideTypeShouldLoseSeparateFlag(outRideEntry)) {
		di |= 0x1000000;
	}

	RCT2_GLOBAL(0xF433DD, uint32) = di;

	return (uint8*)outRideEntry;
}

static void object_type_ride_unload(void *objectEntry)
{
	rct_ride_entry *rideEntry = (rct_ride_entry*)objectEntry;
	rideEntry->name = 0;
	rideEntry->description = 0;
	rideEntry->images_offset = 0;

	for (int i = 0; i < 4; i++) {
		rct_ride_entry_vehicle* rideVehicleEntry = &rideEntry->vehicles[i];

		rideVehicleEntry->base_image_id = 0;
		rideVehicleEntry->var_1C = 0;
		rideVehicleEntry->var_20 = 0;
		rideVehicleEntry->var_24 = 0;
		rideVehicleEntry->var_28 = 0;
		rideVehicleEntry->var_2C = 0;
		rideVehicleEntry->var_30 = 0;
		rideVehicleEntry->var_34 = 0;
		rideVehicleEntry->var_38 = 0;
		rideVehicleEntry->var_3C = 0;
		rideVehicleEntry->var_40 = 0;
		rideVehicleEntry->var_44 = 0;
		rideVehicleEntry->var_48 = 0;
		rideVehicleEntry->var_4C = 0;
		rideVehicleEntry->no_vehicle_images = 0;
		rideVehicleEntry->var_16 = 0;

		if (!(rideVehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_10)) {
			rideVehicleEntry->sprite_width = 0;
			rideVehicleEntry->sprite_height_negative = 0;
			rideVehicleEntry->sprite_height_positive = 0;
		}
		rideVehicleEntry->var_02 = 0;
		rideVehicleEntry->var_03 = 0;
		rideVehicleEntry->peep_loading_positions = 0;
	}

	rideEntry->vehicle_preset_list = NULL;
}

static bool object_type_ride_test(void *objectEntry)
{
	rct_ride_entry_32bit* rideEntry = (rct_ride_entry_32bit*)objectEntry;
	if (rideEntry->excitement_multipler > 75) return false;
	if (rideEntry->intensity_multipler > 75) return false;
	if (rideEntry->nausea_multipler > 75) return false;
	return true;
}

static void object_type_ride_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_ride_entry_32bit *rideEntry = (rct_ride_entry_32bit*)objectEntry;
	int imageId = rideEntry->images_offset;
	if (rideEntry->ride_type[0] == 0xFF) {
		imageId++;
		if (rideEntry->ride_type[1] == 0xFF) {
			imageId++;
		}
	}
	gfx_draw_sprite(dpi, imageId, x - 56, y - 56, 0);
}

static rct_string_id object_type_ride_desc(void *objectEntry)
{
	rct_ride_entry_32bit *rideEntry = (rct_ride_entry_32bit*)objectEntry;

	// Get description
	rct_string_id stringId = rideEntry->description;
	if (!(rideEntry->flags & RIDE_ENTRY_FLAG_SEPARATE_RIDE_NAME) || rideTypeShouldLoseSeparateFlagByRideType(rideEntry->ride_type)) {
		uint8 rideType = rideEntry->ride_type[0];
		if (rideType == 0xFF) {
			rideType = rideEntry->ride_type[1];
			if (rideType == 0xFF) {
				rideType = rideEntry->ride_type[2];
			}
		}
		stringId = 512 + rideType;
	}
	return stringId;
}

static void object_type_ride_reset(void *objectEntry, uint32 entryIndex)
{
	rct_ride_entry *rideEntry = (rct_ride_entry*)objectEntry;

	// After rideEntry is 3 string tables
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_ride_entry));
	rideEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_RIDE, entryIndex, 0);
	rideEntry->description = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_RIDE, entryIndex, 1);

	//TODO: Move to its own function when ride construction window is merged.
	if (gConfigInterface.select_by_track_type) {
		rideEntry->enabledTrackPieces = 0xFFFFFFFFFFFFFFFF;
	}

	object_get_localised_text(&extendedEntryData, OBJECT_TYPE_RIDE, entryIndex, 2);
	rideEntry->vehicle_preset_list = (vehicle_colour_preset_list*)extendedEntryData;

	// If Unknown struct size is 0xFF then there are 32 3 byte structures
	uint8 unknown_size = *extendedEntryData++;
	if (unknown_size != 0xFF) {
		extendedEntryData += unknown_size * 3;
	} else {
		extendedEntryData += 0x60;
	}

	sint8 *peep_loading_positions = (sint8*)extendedEntryData;
	// Peep loading positions variable size
	// 4 different vehicle subtypes are available
	for (int i = 0; i < 4; i++){
		uint16 no_peep_positions = *extendedEntryData++;
		// If no_peep_positions is 0xFF then no_peep_positions is a word
		if (no_peep_positions == 0xFF) {
			no_peep_positions = *((uint16*)extendedEntryData);
			extendedEntryData += 2;
		}
		extendedEntryData += no_peep_positions;
	}

	int images_offset = object_chunk_load_image_directory(&extendedEntryData);
	rideEntry->images_offset = images_offset;

	int cur_vehicle_images_offset = images_offset + 3;

	for (int i = 0; i < 4; i++) {
		rct_ride_entry_vehicle* vehicleEntry = &rideEntry->vehicles[i];

		if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT) {
			int al = 1;
			if (vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_SWINGING) {
				al = 13;
				if ((vehicleEntry->flags_b & (VEHICLE_ENTRY_FLAG_B_5 | VEHICLE_ENTRY_FLAG_B_11)) != (VEHICLE_ENTRY_FLAG_B_5 | VEHICLE_ENTRY_FLAG_B_11)) {
					al = 7;
					if (!(vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_5)) {
						if (!(vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_11)) {
							al = 5;
							if (vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_9) {
								al = 3;
							}
						}
					}
				}
			}
			vehicleEntry->var_03 = al;
			// 0x6DE90B

			al = 0x20;
			if (!(vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_14)) {
				al = 1;
				if (vehicleEntry->flags_b & VEHICLE_ENTRY_FLAG_B_7) {
					if (vehicleEntry->var_11 != 6) {
						al = 2;
						if (!(vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_7)) {
							al = 4;
						}
					}
				}
			}
			if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_12) {
				al = vehicleEntry->special_frames;
			}
			vehicleEntry->var_02 = al;
			// 0x6DE946

			vehicleEntry->var_16 = vehicleEntry->var_02 * vehicleEntry->var_03;
			vehicleEntry->base_image_id = cur_vehicle_images_offset;
			int image_index = vehicleEntry->base_image_id;

			if (vehicleEntry->car_visual != VEHICLE_VISUAL_RIVER_RAPIDS) {
				int b = vehicleEntry->var_16 * 32;

				if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_11) b /= 2;
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_15) b /= 8;

				image_index += b;

				// Incline 25
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_GENTLE_SLOPES) {
					vehicleEntry->var_20 = image_index;
					b = vehicleEntry->var_16 * 72;
					if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_14)
						b = vehicleEntry->var_16 * 16;

					image_index += b;
				}

				// Incline 60
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_STEEP_SLOPES) {
					vehicleEntry->var_24 = image_index;
					b = vehicleEntry->var_16 * 80;
					image_index += b;
				}

				// Verticle
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_VERTICAL_SLOPES) {
					vehicleEntry->var_28 = image_index;
					b = vehicleEntry->var_16 * 116;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_DIAGONAL_SLOPES) {
					vehicleEntry->var_2C = image_index;
					b = vehicleEntry->var_16 * 24;
					image_index += b;
				}

				// Bank
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT_BANKED) {
					vehicleEntry->var_30 = image_index;
					b = vehicleEntry->var_16 * 80;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_INLINE_TWISTS) {
					vehicleEntry->var_34 = image_index;
					b = vehicleEntry->var_16 * 40;
					image_index += b;
				}

				// Track half? Up/Down
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT_TO_GENTLE_SLOPE_BANKED_TRANSITIONS) {
					vehicleEntry->var_38 = image_index;
					b = vehicleEntry->var_16 * 128;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_DIAGONAL_GENTLE_SLOPE_BANKED_TRANSITIONS) {
					vehicleEntry->var_3C = image_index;
					b = vehicleEntry->var_16 * 16;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_GENTLE_SLOPE_BANKED_TRANSITIONS) {
					vehicleEntry->var_40 = image_index;
					b = vehicleEntry->var_16 * 16;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_GENTLE_SLOPE_BANKED_TURNS) {
					vehicleEntry->var_44 = image_index;
					b = vehicleEntry->var_16 * 128;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_FLAT_TO_GENTLE_SLOPE_WHILE_BANKED_TRANSITIONS) {
					vehicleEntry->var_48 = image_index;
					b = vehicleEntry->var_16 * 16;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_CORKSCREWS) {
					vehicleEntry->var_4C = image_index;
					b = vehicleEntry->var_16 * 80;
					image_index += b;
				}

				// Unknown
				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_RESTRAINT_ANIMATION) {
					vehicleEntry->var_1C = image_index;
					b = vehicleEntry->var_16 * 12;
					image_index += b;
				}

				if (vehicleEntry->sprite_flags & VEHICLE_SPRITE_FLAG_14) {
					// Same offset as above???
					vehicleEntry->var_4C = image_index;
					b = vehicleEntry->var_16 * 32;
					image_index += b;
				}
			} else {
				image_index += vehicleEntry->var_16 * 36;
			}
			// No vehicle images
			vehicleEntry->no_vehicle_images = image_index - cur_vehicle_images_offset;

			// Move the offset over this vehicles images. Including peeps
			cur_vehicle_images_offset = image_index + vehicleEntry->no_seating_rows * vehicleEntry->no_vehicle_images;
			// 0x6DEB0D

			if (!(vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_10)) {
				int num_images = cur_vehicle_images_offset - vehicleEntry->base_image_id;
				if (vehicleEntry->flags_a & VEHICLE_ENTRY_FLAG_A_13) {
					num_images *= 2;
				}

				set_vehicle_type_image_max_sizes(vehicleEntry, num_images);
			}

			sint8 no_positions = *peep_loading_positions++;
			if (no_positions == -1) {
				// The no_positions is 16 bit skip over
				peep_loading_positions += 2;
			}
			vehicleEntry->peep_loading_positions = peep_loading_positions;
		}
	}

	// 0x6DEB71
	if (RCT2_GLOBAL(0x9ADAFD, uint8) == 0) {
		for (int i = 0; i < 3; i++) {
			int dl = rideEntry->ride_type[i];
			if (dl == 0xFF) {
				continue;
			}

			uint8 *typeToRideEntryIndexMap = gTypeToRideEntryIndexMap;
			while (dl >= 0) {
				if (*typeToRideEntryIndexMap++ == 0xFF) {
					dl--;
				}
			}

			typeToRideEntryIndexMap--;
			uint8 previous_entry = entryIndex;
			while (typeToRideEntryIndexMap < gTypeToRideEntryIndexMap + countof(gTypeToRideEntryIndexMap)){
				uint8 backup_entry = *typeToRideEntryIndexMap;
				*typeToRideEntryIndexMap++ = previous_entry;
				previous_entry = backup_entry;
			}
		}
	}

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	int di = rideEntry->ride_type[0] | (rideEntry->ride_type[1] << 8) | (rideEntry->ride_type[2] << 16);

	if ((rideEntry->flags & RIDE_ENTRY_FLAG_SEPARATE_RIDE_NAME) && !rideTypeShouldLoseSeparateFlag(rideEntry)) {
		di |= 0x1000000;
	}

	RCT2_GLOBAL(0xF433DD, uint32) = di;
}

static const object_type_vtable object_type_ride_vtable[] = {
	object_type_ride_load,
	object_type_ride_unload,
	object_type_ride_test,
	object_type_ride_paint,
	object_type_ride_desc,
	object_type_ride_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Small Scenery (rct2: 0x006E3466)
///////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 1)
typedef struct rct_large_scenery_entry_32bit {
	uint8 tool_id;			// 0x06
	uint8 flags;			// 0x07
	sint16 price;			// 0x08
	sint16 removal_price;	// 0x0A
	uint32 tiles;			// 0x0C note: 32bit!
	uint8 scenery_tab_id;	// 0x10
	uint8 var_11;
	uint32 text;
	uint32 text_image;
} rct_large_scenery_entry_32bit;
assert_struct_size(rct_large_scenery_entry_32bit, 20);

typedef struct rct_small_scenery_entry_32bit {
	uint32 flags;			// 0x06
	uint8 height;			// 0x0A
	uint8 tool_id;			// 0x0B
	sint16 price;			// 0x0C
	sint16 removal_price;	// 0x0E
	uint32 var_10;			// note: uint32!
	uint8 pad_14[0x06];
	uint8 scenery_tab_id;	// 0x1A
} rct_small_scenery_entry_32bit;
assert_struct_size(rct_small_scenery_entry_32bit, 21);

typedef struct rct_scenery_entry_32bit {
	rct_string_id name;		// 0x00
	uint32 image;			// 0x02
	union {
		rct_small_scenery_entry_32bit small_scenery;
		rct_large_scenery_entry_32bit large_scenery;
		rct_wall_scenery_entry wall;
		rct_path_bit_scenery_entry path_bit;
		rct_banner_scenery_entry banner;
	};
} rct_scenery_entry_32bit;
assert_struct_size(rct_scenery_entry_32bit, 6 + 21);
#pragma pack(pop)

static uint8* object_type_small_scenery_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_scenery_entry_32bit* sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x1C);
	const size_t extendedDataSize = *chunkSize - 0x1C;
	*chunkSize = *chunkSize + sizeof(rct_scenery_entry) - 0x1C;
	assert(*chunkSize > 0);
	rct_scenery_entry* outSceneryEntry = malloc(*chunkSize);
	assert(outSceneryEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outSceneryEntry + sizeof(rct_scenery_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_SMALL_SCENERY, entryIndex, 0);
	sceneryEntry->small_scenery.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF) {
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)) {
			sceneryEntry->small_scenery.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG16){
		outSceneryEntry->small_scenery.var_10 = (uintptr_t)extendedEntryData;
		while (*++extendedEntryData != 0xFF);
		extendedEntryData++;
	} else {
		outSceneryEntry->small_scenery.var_10 = sceneryEntry->small_scenery.var_10;
	}

	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);
	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	outSceneryEntry->name = sceneryEntry->name;
	outSceneryEntry->image = sceneryEntry->image;
	outSceneryEntry->small_scenery.flags = sceneryEntry->small_scenery.flags;
	outSceneryEntry->small_scenery.height = sceneryEntry->small_scenery.height;
	outSceneryEntry->small_scenery.tool_id = sceneryEntry->small_scenery.tool_id;
	outSceneryEntry->small_scenery.price = sceneryEntry->small_scenery.price;
	outSceneryEntry->small_scenery.removal_price = sceneryEntry->small_scenery.removal_price;
	// var10 already set
	// pad_14 not needed set
	outSceneryEntry->small_scenery.scenery_tab_id = sceneryEntry->small_scenery.scenery_tab_id;

	return (uint8*)outSceneryEntry;
}

static void object_type_small_scenery_unload(void *objectEntry)
{
	rct_scenery_entry_32bit *sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	sceneryEntry->name = 0;
	sceneryEntry->image = 0;
	sceneryEntry->small_scenery.var_10 = 0;
	sceneryEntry->small_scenery.scenery_tab_id = 0;
}

static bool object_type_small_scenery_test(void *objectEntry)
{
	rct_scenery_entry_32bit *sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;

	if (sceneryEntry->small_scenery.price <= 0) return false;
	if (sceneryEntry->small_scenery.removal_price > 0) return true;

	// Make sure you don't make a profit when placing then removing.
	if (-sceneryEntry->small_scenery.removal_price > sceneryEntry->small_scenery.price) return false;

	return true;
}

static void object_type_small_scenery_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_scenery_entry_32bit* sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	rct_drawpixelinfo clipDPI;
	if (!clip_drawpixelinfo(&clipDPI, dpi, x - 56, y - 56, 112, 112)) {
		return;
	}

	int imageId = sceneryEntry->image;
	if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG_HAS_PRIMARY_COLOUR) {
		imageId |= 0x20D00000;
		if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG_HAS_SECONDARY_COLOUR) {
			imageId |= 0x92000000;
		}
	}

	x = 56;
	y = sceneryEntry->small_scenery.height / 4 + 78;
	if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG_FULL_TILE) {
		if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG_VOFFSET_CENTRE) {
			y -= 12;
		}
	}

	gfx_draw_sprite(&clipDPI, imageId, x, y, 0);
	if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG10) {
		imageId = sceneryEntry->image + 0x44500004;
		if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG_HAS_SECONDARY_COLOUR) {
			imageId |= 0x92000000;
		}

		gfx_draw_sprite(&clipDPI, imageId, x, y, 0);
	}

	if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG8) {
		imageId = sceneryEntry->image + 4;
		if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG_HAS_SECONDARY_COLOUR) {
			imageId |= 0x92000000;
		}

		gfx_draw_sprite(&clipDPI, imageId, x, y, 0);
	}
}

static rct_string_id object_type_small_scenery_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_small_scenery_reset(void *objectEntry, uint32 entryIndex)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_entry));

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_SMALL_SCENERY, entryIndex, 0);
	sceneryEntry->small_scenery.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF) {
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)) {
			sceneryEntry->small_scenery.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	if (sceneryEntry->small_scenery.flags & SMALL_SCENERY_FLAG16){
		sceneryEntry->small_scenery.var_10 = (uintptr_t)extendedEntryData;
		while (*++extendedEntryData != 0xFF);
		extendedEntryData++;
	}

	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);
	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_small_scenery_vtable[] = {
	object_type_small_scenery_load,
	object_type_small_scenery_unload,
	object_type_small_scenery_test,
	object_type_small_scenery_paint,
	object_type_small_scenery_desc,
	object_type_small_scenery_reset,
};


///////////////////////////////////////////////////////////////////////////////
// Large Scenery (rct2: 0x006B92A7)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_large_scenery_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_scenery_entry_32bit* sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x1A);
	const size_t extendedDataSize = *chunkSize - 0x1A;
	*chunkSize = *chunkSize + sizeof(rct_scenery_entry) - 0x1A;
	assert(*chunkSize > 0);
	rct_scenery_entry* outSceneryEntry = malloc(*chunkSize);
	assert(outSceneryEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outSceneryEntry + sizeof(rct_scenery_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_LARGE_SCENERY, entryIndex, 0);
	sceneryEntry->large_scenery.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF) {
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)) {
			sceneryEntry->large_scenery.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_scenery_entry_32bit);
	if (sceneryEntry->large_scenery.flags & (1 << 2)) {
		outSceneryEntry->large_scenery.text = (rct_large_scenery_text*)extendedEntryData;
		extendedEntryData += 1038;
	}

	outSceneryEntry->large_scenery.tiles = (rct_large_scenery_tile*)extendedEntryData;

	// skip over large scenery tiles
	while (*((uint16*)extendedEntryData) != 0xFFFF){
		extendedEntryData += sizeof(rct_large_scenery_tile);
	}

	extendedEntryData += 2;

	int imageId = object_chunk_load_image_directory(&extendedEntryData);
	if (sceneryEntry->large_scenery.flags & (1 << 2)){
		sceneryEntry->large_scenery.text_image = imageId;

		uint8* edx = (uint8*)outSceneryEntry->large_scenery.text;
		if (!(edx[0xC] & 1)) {
			imageId += edx[0xD] * 4;
		} else{
			imageId += edx[0xD] * 2;
		}
	}
	sceneryEntry->image = imageId;
	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	outSceneryEntry->large_scenery.tool_id = sceneryEntry->large_scenery.tool_id;
	outSceneryEntry->large_scenery.flags = sceneryEntry->large_scenery.flags;
	outSceneryEntry->large_scenery.price = sceneryEntry->large_scenery.price;
	outSceneryEntry->large_scenery.removal_price = sceneryEntry->large_scenery.removal_price;
	// 0x0a tiles is a pointer, already set
	outSceneryEntry->large_scenery.scenery_tab_id = sceneryEntry->large_scenery.scenery_tab_id;
	outSceneryEntry->large_scenery.var_11 = sceneryEntry->large_scenery.var_11;
	// var_12 is a pointer, already set
	outSceneryEntry->large_scenery.text_image = sceneryEntry->large_scenery.text_image;

	return (uint8*)outSceneryEntry;
}

static void object_type_large_scenery_unload(void *objectEntry)
{
	rct_scenery_entry *sceneryEntry = (rct_scenery_entry*)objectEntry;
	sceneryEntry->name = 0;
	sceneryEntry->image = 0;
	sceneryEntry->large_scenery.tiles = 0;
	sceneryEntry->large_scenery.scenery_tab_id = 0;
	sceneryEntry->large_scenery.text = 0;
	sceneryEntry->large_scenery.text_image = 0;
}

static bool object_type_large_scenery_test(void *objectEntry)
{
	rct_scenery_entry_32bit *sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;

	if (sceneryEntry->large_scenery.price <= 0) return false;
	if (sceneryEntry->large_scenery.removal_price > 0) return true;

	// Make sure you don't make a profit when placing then removing.
	if (-sceneryEntry->large_scenery.removal_price > sceneryEntry->large_scenery.price) return false;

	return true;
}

static void object_type_large_scenery_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;

	int imageId = sceneryEntry->image | 0xB2D00000;
	gfx_draw_sprite(dpi, imageId, x, y - 39, 0);
}

static rct_string_id object_type_large_scenery_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_large_scenery_reset(void *objectEntry, uint32 entryIndex)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_entry));

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_LARGE_SCENERY, entryIndex, 0);
	sceneryEntry->large_scenery.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF) {
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)) {
			sceneryEntry->large_scenery.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	if (sceneryEntry->large_scenery.flags & (1 << 2)) {
		sceneryEntry->large_scenery.text = (rct_large_scenery_text *)extendedEntryData;
		extendedEntryData += 1038;
	}

	sceneryEntry->large_scenery.tiles = (rct_large_scenery_tile*)extendedEntryData;

	// skip over large scenery tiles
	while (*((uint16*)extendedEntryData) != 0xFFFF){
		extendedEntryData += sizeof(rct_large_scenery_tile);
	}

	extendedEntryData += 2;

	int imageId = object_chunk_load_image_directory(&extendedEntryData);
	if (sceneryEntry->large_scenery.flags & (1 << 2)){
		sceneryEntry->large_scenery.text_image = imageId;

		uint8* edx = (uint8*)sceneryEntry->large_scenery.text;
		if (!(edx[0xC] & 1)) {
			imageId += edx[0xD] * 4;
		} else{
			imageId += edx[0xD] * 2;
		}
	}
	sceneryEntry->image = imageId;
	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_large_scenery_vtable[] = {
	object_type_large_scenery_load,
	object_type_large_scenery_unload,
	object_type_large_scenery_test,
	object_type_large_scenery_paint,
	object_type_large_scenery_desc,
	object_type_large_scenery_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Wall (rct2: 0x006E5A25)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_wall_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_scenery_entry_32bit* sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x0E);
	const size_t extendedDataSize = *chunkSize - 0x0E;
	*chunkSize = *chunkSize + sizeof(rct_scenery_entry) - 0x0E;
	assert(*chunkSize > 0);
	rct_scenery_entry* outSceneryEntry = malloc(*chunkSize);
	assert(outSceneryEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outSceneryEntry + sizeof(rct_scenery_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_WALLS, entryIndex, 0);
	sceneryEntry->wall.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF){
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)) {
			sceneryEntry->wall.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	outSceneryEntry->name = sceneryEntry->name;
	outSceneryEntry->image = sceneryEntry->image;
	outSceneryEntry->wall.tool_id = sceneryEntry->wall.tool_id;
	outSceneryEntry->wall.flags = sceneryEntry->wall.flags;
	outSceneryEntry->wall.height = sceneryEntry->wall.height;
	outSceneryEntry->wall.flags2 = sceneryEntry->wall.flags2;
	outSceneryEntry->wall.price = sceneryEntry->wall.price;
	outSceneryEntry->wall.scenery_tab_id = sceneryEntry->wall.scenery_tab_id;
	outSceneryEntry->wall.var_0D = sceneryEntry->wall.var_0D;

	return (uint8*)outSceneryEntry;

}

static void object_type_wall_unload(void *objectEntry)
{
	rct_scenery_entry *sceneryEntry = (rct_scenery_entry*)objectEntry;
	sceneryEntry->name = 0;
	sceneryEntry->image = 0;
	sceneryEntry->wall.scenery_tab_id = 0;
}

static bool object_type_wall_test(void *objectEntry)
{
	rct_scenery_entry_32bit *sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	if (sceneryEntry->wall.price <= 0) return false;
	return true;
}

static void object_type_wall_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	rct_drawpixelinfo clipDPI;
	if (!clip_drawpixelinfo(&clipDPI, dpi, x - 56, y - 56, 112, 112)) {
		return;
	}

	int imageId = sceneryEntry->image;
	imageId |= 0x20D00000;
	if (sceneryEntry->wall.flags & WALL_SCENERY_HAS_SECONDARY_COLOUR) {
		imageId |= 0x92000000;
	}

	x = 70;
	y = sceneryEntry->wall.height * 2 + 72;
	gfx_draw_sprite(&clipDPI, imageId, x, y, 0);
	
	if (sceneryEntry->wall.flags & WALL_SCENERY_FLAG2){
		imageId = sceneryEntry->image + 0x44500006;
		gfx_draw_sprite(&clipDPI, imageId, x, y, 0);
	} else if (sceneryEntry->wall.flags & WALL_SCENERY_IS_DOOR){
		imageId++;
		gfx_draw_sprite(&clipDPI, imageId, x, y, 0);
	}
}

static rct_string_id object_type_wall_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_wall_reset(void *objectEntry, uint32 entryIndex)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_entry));

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_WALLS, entryIndex, 0);
	sceneryEntry->wall.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF){
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)) {
			sceneryEntry->wall.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_wall_vtable[] = {
	object_type_wall_load,
	object_type_wall_unload,
	object_type_wall_test,
	object_type_wall_paint,
	object_type_wall_desc,
	object_type_wall_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Banner (rct2: 0x006BA84E)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_banner_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_scenery_entry_32bit* sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x0C);
	const size_t extendedDataSize = *chunkSize - 0x0C;
	*chunkSize = *chunkSize + sizeof(rct_scenery_entry) - 0x0C;
	assert(*chunkSize > 0);
	rct_scenery_entry* outSceneryEntry = malloc(*chunkSize);
	assert(outSceneryEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outSceneryEntry + sizeof(rct_scenery_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_BANNERS, entryIndex, 0);
	sceneryEntry->banner.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF){
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)){
			sceneryEntry->banner.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
	outSceneryEntry->name = sceneryEntry->name;
	outSceneryEntry->image = sceneryEntry->image;
	outSceneryEntry->banner.scrolling_mode = sceneryEntry->banner.scrolling_mode;
	outSceneryEntry->banner.flags = sceneryEntry->banner.flags;
	outSceneryEntry->banner.price = sceneryEntry->banner.price;
	outSceneryEntry->banner.scenery_tab_id = sceneryEntry->banner.scenery_tab_id;

	return (uint8*)outSceneryEntry;
}

static void object_type_banner_unload(void *objectEntry)
{
	rct_scenery_entry *sceneryEntry = (rct_scenery_entry*)objectEntry;
	sceneryEntry->name = 0;
	sceneryEntry->image = 0;
	sceneryEntry->banner.scenery_tab_id = 0;
}

static bool object_type_banner_test(void *objectEntry)
{
	rct_scenery_entry_32bit *sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	if (sceneryEntry->banner.price <= 0) return false;
	return true;
}

static void object_type_banner_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;

	int imageId = sceneryEntry->image | 0x20D00000;
	gfx_draw_sprite(dpi, imageId, x, y, 0);
	gfx_draw_sprite(dpi, imageId + 1, x, y, 0);
}

static rct_string_id object_type_banner_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_banner_reset(void *objectEntry, uint32 entryIndex)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_entry));

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_BANNERS, entryIndex, 0);
	sceneryEntry->banner.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF){
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)){
			sceneryEntry->banner.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_banner_vtable[] = {
	object_type_banner_load,
	object_type_banner_unload,
	object_type_banner_test,
	object_type_banner_paint,
	object_type_banner_desc,
	object_type_banner_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Path (rct2: 0x006A8621)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_path_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_footpath_entry *pathEntry = (rct_footpath_entry*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x0E);
	const size_t extendedDataSize = *chunkSize - 0x0E;
	*chunkSize = *chunkSize + sizeof(rct_footpath_entry) - 0x0E;
	assert(*chunkSize > 0);
	rct_footpath_entry* outPathEntry = malloc(*chunkSize);
	assert(outPathEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outPathEntry + sizeof(rct_footpath_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	pathEntry->string_idx = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_PATHS, entryIndex, 0);

	int imageId = object_chunk_load_image_directory(&extendedEntryData);
	pathEntry->image = imageId;
	pathEntry->bridge_image = imageId + 109;

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	// rct_path_Type has no pointer, its size does not change, safe to memcpy
	memcpy(outPathEntry, pathEntry, sizeof(rct_footpath_entry));

	return (uint8*)outPathEntry;
}

static void object_type_path_unload(void *objectEntry)
{
	rct_footpath_entry *pathEntry = (rct_footpath_entry*)objectEntry;
	pathEntry->string_idx = 0;
	pathEntry->image = 0;
	pathEntry->bridge_image = 0;
}

static bool object_type_path_test(void *objectEntry)
{
	rct_footpath_entry *pathEntry = (rct_footpath_entry*)objectEntry;
	if (pathEntry->var_0A >= 2) return false;
	return true;
}

static void object_type_path_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_footpath_entry *pathEntry = (rct_footpath_entry*)objectEntry;
	gfx_draw_sprite(dpi, pathEntry->image + 71, x - 49, y - 17, 0);
	gfx_draw_sprite(dpi, pathEntry->image + 72, x + 4, y - 17, 0);
}

static rct_string_id object_type_path_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_path_reset(void *objectEntry, uint32 entryIndex)
{
	rct_footpath_entry *pathEntry = (rct_footpath_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_footpath_entry));

	pathEntry->string_idx = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_PATHS, entryIndex, 0);

	int imageId = object_chunk_load_image_directory(&extendedEntryData);
	pathEntry->image = imageId;
	pathEntry->bridge_image = imageId + 109;

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	gFootpathSelectedId = 0;
	// Set the default path for when opening footpath window
	for (int i = 0; i < object_entry_group_counts[OBJECT_TYPE_PATHS]; i++) {
		rct_footpath_entry *pathEntry2 = (rct_footpath_entry*)object_entry_groups[OBJECT_TYPE_PATHS].chunks[i];
		if (pathEntry2 == (rct_footpath_entry*)-1) {
			continue;
		}
		if (!(pathEntry2->flags & 4)) {
			gFootpathSelectedId = i;
			break;
		}
		gFootpathSelectedId = i;
	}
}

static const object_type_vtable object_type_path_vtable[] = {
	object_type_path_load,
	object_type_path_unload,
	object_type_path_test,
	object_type_path_paint,
	object_type_path_desc,
	object_type_path_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Path Item (rct2: 0x006A86E2)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_path_bit_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_scenery_entry_32bit* sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x0E);
	const size_t extendedDataSize = *chunkSize - 0x0E;
	*chunkSize = *chunkSize + sizeof(rct_scenery_entry) - 0x0E;
	assert(*chunkSize > 0);
	rct_scenery_entry* outSceneryEntry = malloc(*chunkSize);
	assert(outSceneryEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outSceneryEntry + sizeof(rct_scenery_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_PATH_BITS, entryIndex, 0);
	sceneryEntry->path_bit.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF) {
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)){
			sceneryEntry->path_bit.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	outSceneryEntry->name = sceneryEntry->name;
	outSceneryEntry->image = sceneryEntry->image;
	outSceneryEntry->path_bit.flags = sceneryEntry->path_bit.flags;
	outSceneryEntry->path_bit.draw_type = sceneryEntry->path_bit.draw_type;
	outSceneryEntry->path_bit.tool_id = sceneryEntry->path_bit.tool_id;
	outSceneryEntry->path_bit.price = sceneryEntry->path_bit.price;
	outSceneryEntry->path_bit.scenery_tab_id = sceneryEntry->path_bit.scenery_tab_id;

	return (uint8*)outSceneryEntry;
}

static void object_type_path_bit_unload(void *objectEntry)
{
	rct_scenery_entry *sceneryEntry = (rct_scenery_entry*)objectEntry;
	sceneryEntry->name = 0;
	sceneryEntry->image = 0;
	sceneryEntry->path_bit.scenery_tab_id = 0;
}

static bool object_type_path_bit_test(void *objectEntry)
{
	rct_scenery_entry_32bit *sceneryEntry = (rct_scenery_entry_32bit*)objectEntry;
	if (sceneryEntry->path_bit.price <= 0) return false;
	return true;
}

static void object_type_path_bit_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	gfx_draw_sprite(dpi, sceneryEntry->image, x - 22, y - 24, 0);
}

static rct_string_id object_type_path_bit_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_path_bit_reset(void *objectEntry, uint32 entryIndex)
{
	rct_scenery_entry* sceneryEntry = (rct_scenery_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_entry));

	sceneryEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_PATH_BITS, entryIndex, 0);
	sceneryEntry->path_bit.scenery_tab_id = 0xFF;
	if (*extendedEntryData != 0xFF) {
		uint8 entry_type, entry_index;
		if (find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index)){
			sceneryEntry->path_bit.scenery_tab_id = entry_index;
		}
	}

	extendedEntryData += sizeof(rct_object_entry);
	sceneryEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_path_bit_vtable[] = {
	object_type_path_bit_load,
	object_type_path_bit_unload,
	object_type_path_bit_test,
	object_type_path_bit_paint,
	object_type_path_bit_desc,
	object_type_path_bit_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Scenery Set (rct2: 0x006B93AA)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_scenery_set_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_scenery_set_entry *scenerySetEntry = (rct_scenery_set_entry*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_set_entry));
	const size_t extendedDataSize = *chunkSize - sizeof(rct_scenery_set_entry);
	*chunkSize = *chunkSize + sizeof(rct_scenery_set_entry) - sizeof(rct_scenery_set_entry);
	assert(*chunkSize > 0);
	rct_scenery_set_entry* outSceneryEntry = malloc(*chunkSize);
	assert(outSceneryEntry != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outSceneryEntry + sizeof(rct_scenery_set_entry));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	scenerySetEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_SCENERY_SETS, entryIndex, 0);
	
	rct_object_entry *entryObjects = NULL;
	uintptr_t eax = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (eax != (uintptr_t)0xFFFFFFFF){
		*((uint16*)eax) = 0;
		entryObjects = (rct_object_entry*)(eax + 2);
	}
	scenerySetEntry->entry_count = 0;
	scenerySetEntry->var_107 = 0;

	for (; *extendedEntryData != 0xFF; extendedEntryData += sizeof(rct_object_entry)) {
		scenerySetEntry->var_107++;

		if (entryObjects != NULL){
			memcpy(entryObjects, extendedEntryData, sizeof(rct_object_entry));
			entryObjects++;
			(*((uint8*)(eax + 1)))++;
		}
		uint8 entry_type;
		uint8 entry_index = 0;
		if (!find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index))
			continue;

		uint16 scenery_entry = entry_index;

		switch (entry_type){
		case OBJECT_TYPE_SMALL_SCENERY:
			break;
		case OBJECT_TYPE_LARGE_SCENERY:
			scenery_entry |= 0x300;
			break;
		case OBJECT_TYPE_WALLS:
			scenery_entry |= 0x200;
			break;
		case OBJECT_TYPE_PATH_BITS:
			scenery_entry |= 0x100;
			break;
		default:
			scenery_entry |= 0x400;
			break;
		}

		scenerySetEntry->scenery_entries[scenerySetEntry->entry_count++] = scenery_entry;
	}

	extendedEntryData++;
	scenerySetEntry->image = object_chunk_load_image_directory(&extendedEntryData);

	memcpy(outSceneryEntry, scenerySetEntry, sizeof(rct_scenery_set_entry));

	return (uint8*)outSceneryEntry;
}

static void object_type_scenery_set_unload(void *objectEntry)
{
	rct_scenery_set_entry *scenerySetEntry = (rct_scenery_set_entry*)objectEntry;
	scenerySetEntry->name = 0;
	scenerySetEntry->image = 0;
	scenerySetEntry->entry_count = 0;
	scenerySetEntry->var_107 = 0;
	memset(scenerySetEntry->scenery_entries, 0, 256);
}

static bool object_type_scenery_set_test(void *objectEntry)
{
	return true;
}

static void object_type_scenery_set_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_scenery_set_entry *scenerySetEntry = (rct_scenery_set_entry*)objectEntry;
	int imageId = scenerySetEntry->image + 0x20600001;
	gfx_draw_sprite(dpi, imageId, x - 15, y - 14, 0);
}

static rct_string_id object_type_scenery_set_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_scenery_set_reset(void *objectEntry, uint32 entryIndex)
{
	rct_scenery_set_entry *scenerySetEntry = (rct_scenery_set_entry*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_scenery_set_entry));

	scenerySetEntry->name = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_SCENERY_SETS, entryIndex, 0);

	rct_object_entry *entryObjects = NULL;
	uintptr_t eax = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (eax != (uintptr_t)0xFFFFFFFF){
		*((uint16*)eax) = 0;
		entryObjects = (rct_object_entry*)(eax + 2);
	}

	scenerySetEntry->entry_count = 0;
	scenerySetEntry->var_107 = 0;

	for (; *extendedEntryData != 0xFF; extendedEntryData += sizeof(rct_object_entry)) {
		scenerySetEntry->var_107++;

		if (entryObjects != NULL){
			memcpy(entryObjects, extendedEntryData, sizeof(rct_object_entry));
			entryObjects++;
			(*((uint16*)(eax + 1)))++;
		}
		uint8 entry_type;
		uint8 entry_index = 0;
		if (!find_object_in_entry_group((rct_object_entry*)extendedEntryData, &entry_type, &entry_index))
			continue;

		uint16 scenery_entry = entry_index;

		switch (entry_type){
		case OBJECT_TYPE_SMALL_SCENERY:
			break;
		case OBJECT_TYPE_LARGE_SCENERY:
			scenery_entry |= 0x300;
			break;
		case OBJECT_TYPE_WALLS:
			scenery_entry |= 0x200;
			break;
		case OBJECT_TYPE_PATH_BITS:
			scenery_entry |= 0x100;
			break;
		default:
			scenery_entry |= 0x400;
			break;
		}

		scenerySetEntry->scenery_entries[scenerySetEntry->entry_count++] = scenery_entry;
	}

	extendedEntryData++;
	scenerySetEntry->image = object_chunk_load_image_directory(&extendedEntryData);
}

static const object_type_vtable object_type_scenery_set_vtable[] = {
	object_type_scenery_set_load,
	object_type_scenery_set_unload,
	object_type_scenery_set_test,
	object_type_scenery_set_paint,
	object_type_scenery_set_desc,
	object_type_scenery_set_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Park Entrance (rct2: 0x00666E42)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_park_entrance_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_entrance_type *entranceType = (rct_entrance_type*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_entrance_type));
	const size_t extendedDataSize = *chunkSize - sizeof(rct_entrance_type);
	*chunkSize = *chunkSize + sizeof(rct_entrance_type) - sizeof(rct_entrance_type);
	assert(*chunkSize > 0);
	rct_entrance_type* outEntranceType = malloc(*chunkSize);
	assert(outEntranceType != NULL);
	uint8 *extendedEntryData = (uint8*)((size_t)outEntranceType + sizeof(rct_entrance_type));
	memcpy(extendedEntryData, origExtendedEntryData, extendedDataSize);

	entranceType->string_idx = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_PARK_ENTRANCE, entryIndex, 0);
	entranceType->image_id = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	memcpy(outEntranceType, entranceType, sizeof(rct_entrance_type));

	return (uint8*)outEntranceType;
}

static void object_type_park_entrance_unload(void *objectEntry)
{
	rct_entrance_type *entranceType = (rct_entrance_type*)objectEntry;
	entranceType->string_idx = 0;
	entranceType->image_id = 0;
}

static bool object_type_park_entrance_test(void *objectEntry)
{
	return true;
}

static void object_type_park_entrance_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	rct_entrance_type *entranceType = (rct_entrance_type*)objectEntry;

	rct_drawpixelinfo clipDPI;
	if (!clip_drawpixelinfo(&clipDPI, dpi, x - 56, y - 56, 112, 112)) {
		return;
	}

	int imageId = entranceType->image_id;
	gfx_draw_sprite(&clipDPI, imageId + 1, 24, 68, 0);
	gfx_draw_sprite(&clipDPI, imageId, 56, 84, 0);
	gfx_draw_sprite(&clipDPI, imageId + 2, 88, 100, 0);
}

static rct_string_id object_type_park_entrance_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_park_entrance_reset(void *objectEntry, uint32 entryIndex)
{
	rct_entrance_type *entranceType = (rct_entrance_type*)objectEntry;
	uint8 *extendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_entrance_type));

	entranceType->string_idx = object_get_localised_text(&extendedEntryData, OBJECT_TYPE_PARK_ENTRANCE, entryIndex, 0);
	entranceType->image_id = object_chunk_load_image_directory(&extendedEntryData);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_park_entrance_vtable[] = {
	object_type_park_entrance_load,
	object_type_park_entrance_unload,
	object_type_park_entrance_test,
	object_type_park_entrance_paint,
	object_type_park_entrance_desc,
	object_type_park_entrance_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Water (rct2: 0x006E6E2A)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_water_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_water_type *waterEntry = (rct_water_type*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + sizeof(rct_water_type));
	const size_t extendedDataSize = *chunkSize - sizeof(rct_water_type);
	*chunkSize = *chunkSize + sizeof(rct_water_type) - sizeof(rct_water_type);
	assert(*chunkSize > 0);
	rct_water_type* outWaterEntry = malloc(*chunkSize);
	assert(outWaterEntry != NULL);
	uint8 *pStringTable = (uint8*)((size_t)outWaterEntry + sizeof(rct_water_type));
	memcpy(pStringTable, origExtendedEntryData, extendedDataSize);

	waterEntry->string_idx = object_get_localised_text(&pStringTable, OBJECT_TYPE_WATER, entryIndex, 0);

	int imageId = object_chunk_load_image_directory(&pStringTable);
	waterEntry->image_id = imageId;
	waterEntry->var_06 = imageId + 1;
	waterEntry->var_0A = imageId + 4;

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	if (RCT2_GLOBAL(0x009ADAFD, uint8) == 0) {
		load_palette();
		gfx_invalidate_screen();
	}

	memcpy(outWaterEntry, waterEntry, sizeof(rct_water_type));

	return (uint8*)outWaterEntry;
}

static void object_type_water_unload(void *objectEntry)
{
	rct_water_type *waterEntry = (rct_water_type*)objectEntry;
	waterEntry->string_idx = 0;
	waterEntry->image_id = 0;
	waterEntry->var_06 = 0;
	waterEntry->var_0A = 0;
}

static bool object_type_water_test(void *objectEntry)
{
	return true;
}

static void object_type_water_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	// Write (no image)
	gfx_draw_string_centred(dpi, 3326, x, y, 0, NULL);
}

static rct_string_id object_type_water_desc(void *objectEntry)
{
	return STR_NONE;
}

static void object_type_water_reset(void *objectEntry, uint32 entryIndex)
{
	rct_water_type *waterEntry = (rct_water_type*)objectEntry;

	uint8 *pStringTable = (uint8*)((size_t)objectEntry + sizeof(rct_water_type));
	waterEntry->string_idx = object_get_localised_text(&pStringTable, OBJECT_TYPE_WATER, entryIndex, 0);

	int imageId = object_chunk_load_image_directory(&pStringTable);
	waterEntry->image_id = imageId;
	waterEntry->var_06 = imageId + 1;
	waterEntry->var_0A = imageId + 4;

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	if (RCT2_GLOBAL(0x009ADAFD, uint8) == 0) {
		load_palette();
		gfx_invalidate_screen();
	}
}

static const object_type_vtable object_type_water_vtable[] = {
	object_type_water_load,
	object_type_water_unload,
	object_type_water_test,
	object_type_water_paint,
	object_type_water_desc,
	object_type_water_reset,
};

///////////////////////////////////////////////////////////////////////////////
// Stex (rct2: 0x0066B355)
///////////////////////////////////////////////////////////////////////////////

static uint8* object_type_stex_load(void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	rct_stex_entry *stexEntry = (rct_stex_entry*)objectEntry;
	const uint8 *origExtendedEntryData = (uint8*)((size_t)objectEntry + 0x08);
	const size_t extendedDataSize = *chunkSize - 0x08;
	*chunkSize = *chunkSize + sizeof(rct_stex_entry) - 0x08;
	assert(*chunkSize > 0);
	rct_stex_entry* outStexEntry = malloc(*chunkSize);
	assert(outStexEntry != NULL);
	uint8 *stringTable = (uint8*)((size_t)outStexEntry + sizeof(rct_stex_entry));
	memcpy(stringTable, origExtendedEntryData, extendedDataSize);
	
	stexEntry->scenario_name = object_get_localised_text(&stringTable, OBJECT_TYPE_SCENARIO_TEXT, entryIndex, 0);
	stexEntry->park_name = object_get_localised_text(&stringTable, OBJECT_TYPE_SCENARIO_TEXT, entryIndex, 1);
	stexEntry->details = object_get_localised_text(&stringTable, OBJECT_TYPE_SCENARIO_TEXT, entryIndex, 2);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}

	memcpy(outStexEntry, stexEntry, sizeof(rct_stex_entry));

	return (uint8*)outStexEntry;
}

static void object_type_stex_unload(void *objectEntry)
{
	rct_stex_entry *stexEntry = (rct_stex_entry*)objectEntry;
	stexEntry->scenario_name = 0;
	stexEntry->park_name = 0;
	stexEntry->details = 0;
}

static bool object_type_stex_test(void *objectEntry)
{
	return true;
}

static void object_type_stex_paint(void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	// Write (no image)
	gfx_draw_string_centred(dpi, 3326, x, y, 0, NULL);
}

static rct_string_id object_type_stex_desc(void *objectEntry)
{
	rct_stex_entry *stexEntry = (rct_stex_entry*)objectEntry;
	return stexEntry->details;
}

static void object_type_stex_reset(void *objectEntry, uint32 entryIndex)
{
	rct_stex_entry *stexEntry = (rct_stex_entry*)objectEntry;
	uint8 *stringTable = (uint8*)((size_t)objectEntry + (size_t)0x08);

	stexEntry->scenario_name = object_get_localised_text(&stringTable, OBJECT_TYPE_SCENARIO_TEXT, entryIndex, 0);
	stexEntry->park_name = object_get_localised_text(&stringTable, OBJECT_TYPE_SCENARIO_TEXT, entryIndex, 1);
	stexEntry->details = object_get_localised_text(&stringTable, OBJECT_TYPE_SCENARIO_TEXT, entryIndex, 2);

	uintptr_t some_pointer = RCT2_GLOBAL(0x9ADAF4, uint32);
	if (some_pointer != (uintptr_t)0xFFFFFFFF){
		*((uint16*)some_pointer) = 0;
	}
}

static const object_type_vtable object_type_stex_vtable[] = {
	object_type_stex_load,
	object_type_stex_unload,
	object_type_stex_test,
	object_type_stex_paint,
	object_type_stex_desc,
	object_type_stex_reset,
};

///////////////////////////////////////////////////////////////////////////////

static const object_type_vtable * const object_type_vtables[] = {
	object_type_ride_vtable,
	object_type_small_scenery_vtable,
	object_type_large_scenery_vtable,
	object_type_wall_vtable,
	object_type_banner_vtable,
	object_type_path_vtable,
	object_type_path_bit_vtable,
	object_type_scenery_set_vtable,
	object_type_park_entrance_vtable,
	object_type_water_vtable,
	object_type_stex_vtable
};

uint8* object_load(int type, void *objectEntry, uint32 entryIndex, int *chunkSize)
{
	assert(type >= OBJECT_TYPE_RIDE && type <= OBJECT_TYPE_SCENARIO_TEXT);
	const object_type_vtable *vtable = object_type_vtables[type];
	return vtable->load(objectEntry, entryIndex, chunkSize);
}

void object_unload(int type, void *objectEntry)
{
	assert(type >= OBJECT_TYPE_RIDE && type <= OBJECT_TYPE_SCENARIO_TEXT);
	const object_type_vtable *vtable = object_type_vtables[type];
	vtable->unload(objectEntry);
}

bool object_test(int type, void *objectEntry)
{
	assert(type >= OBJECT_TYPE_RIDE && type <= OBJECT_TYPE_SCENARIO_TEXT);
	const object_type_vtable *vtable = object_type_vtables[type];
	return vtable->test(objectEntry);
}

void object_paint(int type, void *objectEntry, rct_drawpixelinfo *dpi, sint32 x, sint32 y)
{
	assert(type >= OBJECT_TYPE_RIDE && type <= OBJECT_TYPE_SCENARIO_TEXT);
	const object_type_vtable *vtable = object_type_vtables[type];
	vtable->paint(objectEntry, dpi, x, y);
}

rct_string_id object_desc(int type, void *objectEntry)
{
	assert(type >= OBJECT_TYPE_RIDE && type <= OBJECT_TYPE_SCENARIO_TEXT);
	const object_type_vtable *vtable = object_type_vtables[type];
	return vtable->desc(objectEntry);
}

void object_reset(int type, void *objectEntry, uint32 entryIndex)
{
	assert(type >= OBJECT_TYPE_RIDE && type <= OBJECT_TYPE_SCENARIO_TEXT);
	const object_type_vtable *vtable = object_type_vtables[type];
	vtable->reset(objectEntry, entryIndex);
}

/**
 *
 *  rct2: 0x006A9428
 */
int object_get_scenario_text(rct_object_entry *entry)
{
	rct_object_entry *installedObject = gInstalledObjects;

	installedObject = object_list_find(entry);

	if (installedObject == NULL){
		log_error("Object not found: %.8s", entry->name);
		return 0;
	}

	char path[MAX_PATH];
	char *objectPath = (char*)installedObject + 16;
	substitute_path(path, gRCT2AddressObjectDataPath, objectPath);

	rct_object_entry openedEntry;
	SDL_RWops* rw = SDL_RWFromFile(path, "rb");
	if (rw != NULL) {
		SDL_RWread(rw, &openedEntry, sizeof(rct_object_entry), 1);
		if (object_entry_compare(&openedEntry, entry)) {

			// Skip over the object entry
			char *pos = (char*)installedObject + sizeof(rct_object_entry);
			// Skip file name
			while (*pos++);

			// Read chunk
			int chunkSize = *((uint32*)pos);

			uint8 *chunk;
			if (chunkSize == 0xFFFFFFFF) {
				chunk = (uint8*)malloc(0x600000);
				chunkSize = sawyercoding_read_chunk(rw, chunk);
				chunk = realloc(chunk, chunkSize);
			}
			else {
				chunk = (uint8*)malloc(chunkSize);
				sawyercoding_read_chunk_with_size(rw, chunk, chunkSize);
			}
			SDL_RWclose(rw);

			// Calculate and check checksum
			if (object_calculate_checksum(&openedEntry, chunk, chunkSize) != openedEntry.checksum) {
				log_error("Opened object failed calculated checksum.");
				free(chunk);
				return 0;
			}

			if (!object_test(openedEntry.flags & 0x0F, chunk)) {
				// This is impossible for STEX entries to fail.
				log_error("Opened object failed paint test.");
				free(chunk);
				return 0;
			}

			// Save the real total images.
			int total_no_images = gTotalNoImages;

			// This is being changed to force the images to be loaded into a different
			// image id.
			chunk = object_load(openedEntry.flags & 0x0F, chunk, 0, &chunkSize);
			gTotalNoImages = 0x726E;
			gStexTempChunk = (rct_stex_entry*)chunk;
			// Not used anywhere.
			RCT2_GLOBAL(RCT2_ADDRESS_SCENARIO_TEXT_TEMP_OBJECT, rct_object_entry) = openedEntry;

			// Tell text to be loaded into a different address
			RCT2_GLOBAL(0x009ADAFC, uint8) = 255;
			memcpy(gTempObjectLoadName, openedEntry.name, 8);
			// Not used??
			RCT2_GLOBAL(0x009ADAFD, uint8) = 1;
			// Tell text to be loaded into normal address
			RCT2_GLOBAL(0x009ADAFC, uint8) = 0;
			// Not used??
			RCT2_GLOBAL(0x009ADAFD, uint8) = 0;
			gTotalNoImages = total_no_images;
			return 1;
		}
		log_error("Opened object didn't match.");
		SDL_RWclose(rw);
		return 0;
	}
	log_error("File failed to open.");
	return 0;
}

/**
 *
 *  rct2: 0x006A982D
 */
void object_free_scenario_text()
{
	if (gStexTempChunk != NULL) {
		free(gStexTempChunk);
		gStexTempChunk = NULL;
	}
}

uintptr_t object_get_length(const rct_object_entry *entry)
{
	return (uintptr_t)object_get_next(entry) - (uintptr_t)entry;
}

rct_object_entry *object_get_next(const rct_object_entry *entry)
{
	uint8 *pos = (uint8*)entry;

	// Skip sizeof(rct_object_entry)
	pos += 16;

	// Skip filename
	while (*pos++);

	// Skip no of images
	pos += 4;

	// Skip name
	while (*pos++);

	// Skip size of chunk
	pos += 4;

	// Skip required objects
	pos += *pos * 16 + 1;

	// Skip theme objects
	pos += *pos * 16 + 1;

	// Skip
	pos += 4;

	return (rct_object_entry*)pos;
}

char *object_get_name(rct_object_entry *entry)
{
	uint8 *pos = (uint8*)entry;

	// Skip sizeof(rct_object_entry)
	pos += 16;

	// Skip filename
	while (*pos++);

	// Skip no of images
	pos += 4;

	return (char *)pos;
}
