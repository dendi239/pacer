#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Possible values of the "fixType" field of #uGnssDecUbxNavPvt_t.
 */
typedef enum {
  U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_NO_FIX = 0,
  U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_DEAD_RECKONING_ONLY = 1,
  U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_2D = 2,
  U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_3D = 3,
  U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_GNSS_PLUS_DEAD_RECKONING = 4,
  U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_TIME_ONLY = 5
} uGnssDecUbxNavPvtFixType_t;

/** UBX-NAV-PVT message structure; the naming and type of each
 * element follows that of the interface manual.
 */
typedef struct {
  uint32_t iTOW; /**< GPS time of week of the
                      navigation epoch
                      in milliseconds. */
  uint16_t year; /**< year (UTC); to obtain this and
                      the other time-related fields
                      in this structure as a Unix-based
                      UTC timestamp, see
                      uGnssDecUbxNavPvtGetTimeUtc(). */
  uint8_t month; /**< month, range 1 to 12 (UTC). */
  uint8_t day;   /**< day of month, range 1 to 31 (UTC). */
  uint8_t hour;  /**< hour of day, range 0 to 23 (UTC). */
  uint8_t min;   /**< minute of hour, range 0 to 59 (UTC). */
  uint8_t sec;   /**< seconds of minute, range 0 to 60 (UTC). */
  uint8_t valid; /**< validity flags, see
                      #uGnssDecUbxNavPvtValid_t. */
  uint32_t tAcc; /**< time accuracy estimate in
                      nanoseconds. */
  int32_t nano;  /**< fractional seconds part of UTC
                      time in nanoseconds. */
  uGnssDecUbxNavPvtFixType_t fixType; /**< the fix type achieved. */
  uint8_t flags;                      /**< see #uGnssDecUbxNavPvtFlags_t. */
  uint8_t flags2;                     /**< see #uGnssDecUbxNavPvtFlags2_t. */
  uint8_t numSV;                      /**< the number of satellites used. */
  int32_t lon;                        /**< longitude in degrees times 1e7. */
  int32_t lat;                        /**< latitude in degrees times 1e7. */
  int32_t height;                     /**< height above ellipsoid in mm. */
  int32_t hMSL;                       /**< height above mean sea level in mm. */
  uint32_t hAcc;    /**< horizontal accuracy estimate in mm. */
  uint32_t vAcc;    /**< vertical accuracy estimate in mm. */
  int32_t velN;     /**< NED north velocity in mm/second. */
  int32_t velE;     /**< NED east velocity in mm/second. */
  int32_t velD;     /**< NED down velocity in mm/second. */
  int32_t gSpeed;   /**< 2D ground speed in mm/second. */
  int32_t headMot;  /**< 2D heading of motion in degrees times 1e5. */
  uint32_t sAcc;    /**< speed accuracy estimate in mm/second. */
  uint32_t headAcc; /**< heading accuracy estimate (motion and
                         vehicle) in degrees times 1e5. */
  uint16_t pDOP;    /**< position DOP times 100. */
  uint16_t flags3;  /**< see #uGnssDecUbxNavPvtFlags3_t. */
  int32_t headVeh;  /**< if the #U_GNSS_DEC_UBX_NAV_PVT_FLAGS_HEAD_VEH_VALID
                         bit of the flags field is set
                         then this is the 2D vehicle
                         heading in degrees times 1e5,
                         else it is set to the same
                         value as headMot. */
  int16_t magDec;   /**< if the #U_GNSS_DEC_UBX_NAV_PVT_VALID_MAG
                         bit of the valid field is set then this
                         is the magnetic declination in degrees
                         times 100; only supported on ADR 4.10
                         and later. */
  uint16_t magAcc;  /**< if the #U_GNSS_DEC_UBX_NAV_PVT_VALID_MAG
                         bit of the valid field is set then this
                         is the accuracy of the magnetic
                         declination in degrees times 100; only
                         supported on ADR 4.10 and later. */
} uGnssDecUbxNavPvt_t;

#ifdef __cplusplus
}
#endif

/** @}*/

// End of file

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return 1;
  }

  FILE *f = fopen(argv[1], "rb");

  //   "/Users/denys/Downloads/1749283873879948155.dat";

  int64_t timestamp;
  uGnssDecUbxNavPvt_t data;

  // TODO: put me into GPSSource.

  while (!feof(f)) {
    fread(&timestamp, sizeof(timestamp), 1, f);
    fread(&data, sizeof(data), 1, f);

    printf("Timestamp: %llu, Lat: %d, Lon: %d, Alt: %d, Speed: %d\n", timestamp,
           data.lat, data.lon, data.height, data.gSpeed);
  }
}
