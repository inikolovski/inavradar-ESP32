#include <Arduino.h>
#include <esp_system.h>
#include <lib/MSP.h>
#include <lib/LoRa.h>

#include <SSD1306.h>
#include <EEPROM.h>
#include <main.h>
#include <pixel.h>
#include <math.h>
#include <cmath>
#include <BluetoothSerial.h>
#include <lib/cli.h>

// -------- VARS

config_t cfg;
system_t sys;
stats_t stats;
MSP msp;

#ifdef USE_CLI
CLI * cli = new CLI;
CLI * cliBT = new CLI;
#endif

#ifdef USE_BT
BluetoothSerial SerialBT;
#endif

msp_radar_pos_t radarPos;

curr_t curr;
peer_t peers[LORA_NODES_MAX];

air_type0_t air_0;
air_type1_t air_1;
air_type2_t air_2;
air_type3_t air_3;

air_type1_t * air_r1;
air_type2_t * air_r2;
air_type3_t * air_r3;

char host_name[3][5]={"NoFC", "iNav", "Beta"};
char host_state[2][5]={"", "ARM"};
char peer_slotname[9][3]={"X", "A", "B", "C", "D", "E", "F", "G", "H"};

SSD1306 display(0x3c, 4, 15);

// -------- EEPROM / CONFIG

void config_save() {
    for(size_t i = 0; i < sizeof(cfg); i++) {
        char data = ((char *)&cfg)[i];
        EEPROM.write(i, data);
    }
    EEPROM.commit();
}

void config_init() {

    size_t size = sizeof(cfg);
    EEPROM.begin(size * 2);

    for(size_t i = 0; i < size; i++)  {
        char data = EEPROM.read(i);
        ((char *)&cfg)[i] = data;
    }

    if (cfg.version != VERSION_CONFIG || FORCE_DEFAULT_PROFILE) { // write default config
        cfg.version = VERSION_CONFIG;
        cfg.profile_id = CFG_PROFILE_DEFAULT_ID;
        strcpy(cfg.profile_name, "Default");

        cfg.lora_frequency = 433E6; // 433E6, 868E6, 915E6
        cfg.lora_bandwidth = 250000;
        cfg.lora_coding_rate = 5;
        cfg.lora_spreading_factor = 9;
        cfg.lora_power = 20;

        cfg.lora_nodes_max = LORA_NODES_MAX;
        cfg.lora_slot_spacing = 125;
        cfg.lora_timing_delay = -70;
        cfg.msp_after_tx_delay = 85;

        cfg.display_enable = 1;
        cfg.io_pin_led = 2;

        config_save();
    }
    else {
        Serial.println("Configuration file found!");
    }
}


// -------- SYSTEM

int count_peers(bool active = 0) {
    int j = 0;
    for (int i = 0; i < cfg.lora_nodes_max; i++) {
        if (active == 1) {
            if ((peers[i].id > 0) && !peers[i].lost) {
                j++;
            }
        }
        else {
            if (peers[i].id > 0) {
                j++;
            }
        }
    }
    return j;
}

void reset_peers() {
    sys.now_sec = millis();
    for (int i = 0; i < cfg.lora_nodes_max; i++) {
        peers[i].id = 0;
        peers[i].host = 0;
        peers[i].state = 0;
        peers[i].lost = 0;
        peers[i].broadcast = 0;
        peers[i].lq_updated = sys.now_sec;
        peers[i].lq_tick = 0;
        peers[i].lq = 0;
        peers[i].updated = 0;
        peers[i].rssi = 0;
        peers[i].distance = 0;
        peers[i].direction = 0;
        peers[i].relalt = 0;
        strcpy(peers[i].name, "");
    }
}

void pick_id() {
    curr.id = 0;
    for (int i = 0; i < cfg.lora_nodes_max; i++) {
        if ((peers[i].id == 0) && (curr.id == 0)) {
            curr.id = i + 1;
        }
    }
}

void resync_tx_slot(int16_t delay) {
    bool startnow = 0;
    for (int i = 0; (i < cfg.lora_nodes_max) && (startnow == 0); i++) { // Resync
        if (peers[i].id > 0) {
            sys.lora_next_tx = peers[i].updated + (curr.id - peers[i].id) * cfg.lora_slot_spacing + sys.lora_cycle + delay;
            startnow = 1;
        }
    }
}

// ----------------------------------------------------------------------------- calc gps distance

double deg2rad(double deg) {
  return (deg * M_PI / 180);
}

double rad2deg(double rad) {
  return (rad * 180 / M_PI);
}

/**
 * Returns the distance between two points on the Earth.
 * Direct translation from http://en.wikipedia.org/wiki/Haversine_formula
 * @param lat1d Latitude of the first point in degrees
 * @param lon1d Longitude of the first point in degrees
 * @param lat2d Latitude of the second point in degrees
 * @param lon2d Longitude of the second point in degrees
 * @return The distance between the two points in meters
 */

double gpsDistanceBetween(double lat1d, double lon1d, double lat2d, double lon2d) {
  double lat1r, lon1r, lat2r, lon2r, u, v;
  lat1r = deg2rad(lat1d);
  lon1r = deg2rad(lon1d);
  lat2r = deg2rad(lat2d);
  lon2r = deg2rad(lon2d);
  u = sin((lat2r - lat1r)/2);
  v = sin((lon2r - lon1r)/2);
  return 2.0 * 6371000 * asin(sqrt(u * u + cos(lat1r) * cos(lat2r) * v * v));
}

/*
double gpsDistanceBetween(double lat1, double long1, double lat2, double long2)
{
  // returns distance in meters between two positions, both specified
  // as signed decimal-degrees latitude and longitude. Uses great-circle
  // distance computation for hypothetical sphere of radius 6372795 meters.
  // Because Earth is no exact sphere, rounding errors may be up to 0.5%.
  // Courtesy of Maarten Lamers
  double delta = radians(long1-long2);
  double sdlong = sin(delta);
  double cdlong = cos(delta);
  lat1 = radians(lat1);
  lat2 = radians(lat2);
  double slat1 = sin(lat1);
  double clat1 = cos(lat1);
  double slat2 = sin(lat2);
  double clat2 = cos(lat2);
  delta = (clat1 * slat2) - (slat1 * clat2 * cdlong);
  delta = sq(delta);
  delta += sq(clat2 * sdlong);
  delta = sqrt(delta);
  double denom = (slat1 * slat2) + (clat1 * clat2 * cdlong);
  delta = atan2(delta, denom);
  return delta * 6372795;
}

*/

double gpsCourseTo(double lat1, double long1, double lat2, double long2)
{
  // returns course in degrees (North=0, West=270) from position 1 to position 2,
  // both specified as signed decimal-degrees latitude and longitude.
  // Because Earth is no exact sphere, calculated course may be off by a tiny fraction.
  // Courtesy of Maarten Lamers
  double dlon = radians(long2-long1);
  lat1 = radians(lat1);
  lat2 = radians(lat2);
  double a1 = sin(dlon) * cos(lat2);
  double a2 = sin(lat1) * cos(lat2) * cos(dlon);
  a2 = cos(lat1) * sin(lat2) - a2;
  a2 = atan2(a1, a2);
  if (a2 < 0.0)
  {
    a2 += TWO_PI;
  }
  return degrees(a2);
}

// -------- LoRa


void lora_send() {

    if (sys.lora_tick % 8 == 0) {

        if (sys.lora_tick % 16 == 0) {
            air_3.id = curr.id;
            air_3.type = 3;
            air_3.vbat = curr.fcanalog.vbat; // 1 to 255 (V x 10)
            air_3.mah = curr.fcanalog.mAhDrawn;
            air_3.rssi = curr.fcanalog.rssi; // 0 to 1023
            air_3.temp1 = 0;
            air_3.temp2 = 0;

            while (!LoRa.beginPacket()) {  }
            LoRa.write((uint8_t*)&air_3, sizeof(air_3));
            LoRa.endPacket(false);
        }
        else {
            air_2.id = curr.id;
            air_2.type = 2;
            air_2.host = curr.host;
            air_2.state = curr.state;
            strncpy(air_2.name, curr.name, LORA_NAME_LENGTH);
            air_2.temp1 = 0;

            while (!LoRa.beginPacket()) {  }
            LoRa.write((uint8_t*)&air_2, sizeof(air_2));
            LoRa.endPacket(false);
            }
    }
    else {

        if (sys.lora_tick % 2 == 0) {
            air_0.id = curr.id;
            air_0.type = 0;
            air_0.lat = curr.gps.lat / 100; // From XX.1234567 to XX.12345
            air_0.lon = curr.gps.lon / 100; // From XX.1234567 to XX.12345
            air_0.alt = curr.gps.alt; // m
            air_0.heading = curr.gps.groundCourse / 10;  // From degres x 10 to degres

            while (!LoRa.beginPacket()) {  }
            LoRa.write((uint8_t*)&air_0, sizeof(air_0));
            LoRa.endPacket(false);
        }
        else {
            air_1.id = curr.id;
            air_1.type = 1;
            air_1.lat = curr.gps.lat / 100; // From XX.1234567 to XX.12345
            air_1.lon = curr.gps.lon / 100; // From XX.1234567 to XX.12345
            air_1.alt = curr.gps.alt; // m
            air_1.speed = curr.gps.groundSpeed / 100; // From cm/s to m/s
            air_1.broadcast = 0;

            while (!LoRa.beginPacket()) {  }
            LoRa.write((uint8_t*)&air_1, sizeof(air_1));
            LoRa.endPacket(false);
        }
    }
}

void lora_receive(int packetSize) {

    if (packetSize == 0) return;

    sys.lora_last_rx = millis();
    sys.lora_last_rx -= (stats.last_tx_duration > 0 ) ? stats.last_tx_duration : 0; // RX time should be the same as TX time

    sys.last_rssi = LoRa.packetRssi();
    sys.ppsc++;

    LoRa.readBytes((uint8_t *)&air_0, packetSize);

    uint8_t id = air_0.id - 1;
    sys.air_last_received_id = air_0.id;
    peers[id].id = sys.air_last_received_id;
    peers[id].lq_tick++;
    peers[id].lost = 0;
    peers[id].updated = sys.lora_last_rx;
    peers[id].rssi = sys.last_rssi;

    if (air_0.type == 0) { // Type 0 packet

        peers[id].gps.lat = air_0.lat * 100; // From XX.12345 to XX.1234500
        peers[id].gps.lon = air_0.lon * 100; // From XX.12345 to XX.1234500
        peers[id].gps.alt = air_0.alt; // m
        peers[id].gps.groundCourse = air_0.heading * 10; // From degres to degres x 10
    }
    else if (air_0.type == 1) { // Type 1 packet

        air_r1 = (air_type1_t*)&air_0;

        peers[id].gps.lat = (*air_r1).lat * 100; // From XX.12345 to XX.1234500
        peers[id].gps.lon = (*air_r1).lon * 100; // From XX.12345 to XX.1234500
        peers[id].gps.alt = (*air_r1).alt; // m
        peers[id].gps.groundSpeed = (*air_r1).speed * 100; // From m/s to cm/s
        peers[id].broadcast = (*air_r1).broadcast;
    }
    else if (air_0.type == 2) { // Type 2 packet

        air_r2 = (air_type2_t*)&air_0;

        peers[id].host = (*air_r2).host;
        peers[id].state = (*air_r2).state;
        strncpy(peers[id].name, (*air_r2).name, LORA_NAME_LENGTH);
        peers[id].name[LORA_NAME_LENGTH] = 0;
    }
    else if (air_0.type == 3) { // Type 3 packet

        air_r3 = (air_type3_t*)&air_0;

        peers[id].fcanalog.vbat = (*air_r3).vbat;
        peers[id].fcanalog.mAhDrawn = (*air_r3).mah;
        peers[id].fcanalog.rssi = (*air_r3).rssi;
    }

    if ((air_0.type == 0 || air_0.type == 1) && peers[id].gps.lat != 0 && peers[id].gps.lon != 0 && peers[id].gps.alt != 0) {  // Save the last known coordinates
        peers[id].gpsrec.lat = peers[id].gps.lat;
        peers[id].gpsrec.lon = peers[id].gps.lon;
        peers[id].gpsrec.alt = peers[id].gps.alt;
        peers[id].gpsrec.groundCourse = peers[id].gps.groundCourse;
        peers[id].gpsrec.groundSpeed = peers[id].gps.groundSpeed;
    }

    sys.num_peers = count_peers();

    if ((sys.air_last_received_id == curr.id) && (sys.phase > MODE_LORA_SYNC) && !sys.lora_no_tx) { // Slot conflict
        uint32_t cs1 = peers[id].name[0] + peers[id].name[1] * 26 + peers[id].name[2] * 26 * 26 ;
        uint32_t cs2 = curr.name[0] + curr.name[1] * 26 + curr.name[2] * 26 * 26 + 1;
        if (cs1 < cs2) { // Pick another slot
            sprintf(sys.message, "%s", "ID CONFLICT");
            pick_id();
            resync_tx_slot(cfg.lora_timing_delay);
        }
    }
}

void lora_init() {

    SPI.begin(5, 19, 27, 18);
    LoRa.setPins(SS, RST, DI0);

    if (!LoRa.begin(cfg.lora_frequency)) {
        while (1);
    }

    LoRa.sleep();
    LoRa.setSignalBandwidth(cfg.lora_bandwidth);
    LoRa.setCodingRate4(cfg.lora_coding_rate);
    LoRa.setSpreadingFactor(cfg.lora_spreading_factor);
    LoRa.setTxPower(cfg.lora_power, 1);
    LoRa.setOCP(250);
    LoRa.idle();
    LoRa.onReceive(lora_receive);
    LoRa.enableCrc();
}

// ----------------------------------------------------------------------------- Display

void display_init() {
    pinMode(16, OUTPUT);
    pinMode(2, OUTPUT);
    digitalWrite(16, LOW);
    delay(50);
    digitalWrite(16, HIGH);
    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

void display_draw() {
    display.clear();
    int j = 0;
    int line;

    if (sys.display_page == 0) {

        display.setFont(ArialMT_Plain_24);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(26, 11, String(curr.gps.numSat));
        display.drawString(13, 42, String(sys.num_peers_active + 1));
        display.drawString (125, 11, String(peer_slotname[curr.id]));

        display.setFont(ArialMT_Plain_10);

        display.drawString (126, 29, "_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ ");
        display.drawString (107, 44, String(stats.percent_received));
        display.drawString(107, 54, String(sys.last_rssi));

        display.setTextAlignment (TEXT_ALIGN_CENTER);
        display.drawString (64, 0, String(sys.message));

        display.setTextAlignment (TEXT_ALIGN_LEFT);
        display.drawString (55, 12, String(curr.name));
        display.drawString (27, 23, "SAT");
        display.drawString (108, 44, "%E");

        display.drawString(21, 54, String(sys.pps) + "p/s");
        display.drawString (109, 54, "dB");
        display.drawString (55, 23, String(host_name[curr.host]));
        display.drawString (55, 44, "P" + String(cfg.profile_id));

        for (int i = 0; i < cfg.lora_nodes_max; i++) {

            if (peers[i].id > 0 && peers[i].updated > millis() - sys.lora_cycle) {
                display.drawString (44 + i * 8, 54, String(peer_slotname[peers[i].id]));
            }
        }

        display.drawString (15, 44, "Nod/" + String(cfg.lora_nodes_max));

        if (curr.gps.fixType == 1) display.drawString (27, 12, "2D");
        if (curr.gps.fixType == 2) display.drawString (27, 12, "3D");
    }

    else if (sys.display_page == 1) {

        long pos[LORA_NODES_MAX];
        long diff;

        display.setFont (ArialMT_Plain_10);
        display.setTextAlignment (TEXT_ALIGN_LEFT);
        display.drawHorizontalLine(0, 11, 128);

        for (int i = 0; i < cfg.lora_nodes_max ; i++) {
            if (peers[i].id > 0 && !peers[i].lost) {
                diff = sys.lora_last_tx - peers[i].updated;
                if (diff > 0 && diff < sys.lora_cycle) {
                    pos[i] = 128 - round(128 * diff / sys.lora_cycle);
                }
            }
            else {
                pos[i] = -1;
            }
        }

        int rect_l = stats.last_tx_duration * 128 / sys.lora_cycle;

        for (int i = 0; i < cfg.lora_nodes_max; i++) {

            display.setTextAlignment (TEXT_ALIGN_LEFT);

            if (pos[i] > -1) {
                display.drawRect(pos[i], 0, rect_l, 12);
                display.drawString (pos[i] + 2, 0, String(peer_slotname[peers[i].id]));
            }

            if (peers[i].id > 0 && j < 4) {
                line = j * 9 + 14;

                display.drawString (0, line, String(peer_slotname[peers[i].id]));
                display.drawString (12, line, String(peers[i].name));
                display.drawString (60, line, String(host_name[peers[i].host]));
                display.setTextAlignment (TEXT_ALIGN_RIGHT);

                if (peers[i].lost) { // Peer timed out
                    display.drawString (127, line, "L:" + String((int)((sys.lora_last_tx - peers[i].updated) / 1000)) + "s" );
                }
                else {
                    if (sys.lora_last_tx > peers[i].updated) {
                        display.drawString (119, line, String(sys.lora_last_tx - peers[i].updated));
                        display.drawString (127, line, "-");
                    }
                    else {
                        display.drawString (119, line, String(sys.lora_cycle + sys.lora_last_tx - peers[i].updated));
                        display.drawString (127, line, "+");

                    }
                }
            j++;
            }
        }
    }

    else if (sys.display_page == 2) {

        display.setFont (ArialMT_Plain_10);
        display.setTextAlignment (TEXT_ALIGN_LEFT);
        display.drawString(0, 0, "LORA TX");
        display.drawString(0, 10, "MSP");
        display.drawString(0, 20, "OLED");
        display.drawString(0, 30, "CYCLE");
        display.drawString(0, 40, "SLOTS");
        display.drawString(0, 50, "UPTIME");

        display.drawString(112, 0, "ms");
        display.drawString(112, 10, "ms");
        display.drawString(112, 20, "ms");
        display.drawString(112, 30, "ms");
        display.drawString(112, 40, "ms");
        display.drawString(112, 50, "s");

        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString (111, 0, String(stats.last_tx_duration));
        display.drawString (111, 10, String(stats.last_msp_duration[0]) + " / " + String(stats.last_msp_duration[1]) + " / " + String(stats.last_msp_duration[2]) + " / " + String(stats.last_msp_duration[3]));
        display.drawString (111, 20, String(stats.last_oled_duration));
        display.drawString (111, 30, String(sys.lora_cycle));
        display.drawString (111, 40, String(cfg.lora_nodes_max) + " x " + String(cfg.lora_slot_spacing));
        display.drawString (111, 50, String((int)millis() / 1000));

    }
    else if (sys.display_page >= 3) {

        int i = constrain(sys.display_page - 3, 0, cfg.lora_nodes_max - 1);
        bool iscurrent = (i + 1 == curr.id);

        display.setFont(ArialMT_Plain_24);
        display.setTextAlignment (TEXT_ALIGN_LEFT);
        display.drawString (0, 0, String(peer_slotname[i + 1]));

        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);

        if (iscurrent) {
           display.drawString (128, 0, String(curr.name));
        }
        else {
            display.drawString (128, 0, String(peers[i].name));
        }

        display.setTextAlignment (TEXT_ALIGN_LEFT);
        display.setFont (ArialMT_Plain_10);

        if (peers[i].id > 0 || iscurrent) {

            if (peers[i].lost && !iscurrent) { display.drawString (19, 0, "LOST"); }
                else if (peers[i].lq == 0 && !iscurrent) { display.drawString (19, 0, "x"); }
                else if (peers[i].lq == 1) { display.drawXbm(19, 2, 8, 8, icon_lq_1); }
                else if (peers[i].lq == 2) { display.drawXbm(19, 2, 8, 8, icon_lq_2); }
                else if (peers[i].lq == 3) { display.drawXbm(19, 2, 8, 8, icon_lq_3); }
                else if (peers[i].lq == 4) { display.drawXbm(19, 2, 8, 8, icon_lq_4); }

                if (iscurrent) {
                    display.drawString (19, 0, "HOST");
                    display.drawString (19, 12, String(host_name[curr.host]));
                }
                else {
                    if (!peers[i].lost) {
                        display.drawString (28, 0, String(peers[i].rssi) + "db");
                    }
                    display.drawString (19, 12, String(host_name[peers[i].host]));
                }

                if (iscurrent) {
                    display.drawString (50, 12, String(host_state[curr.state]));
                }
                else {
                    display.drawString (50, 12, String(host_state[peers[i].state]));
                }

                display.setTextAlignment (TEXT_ALIGN_RIGHT);

                if (iscurrent) {
                    display.drawString (128, 24, "LA " + String((float)curr.gps.lat / 10000000, 6));
                    display.drawString (128, 34, "LO "+ String((float)curr.gps.lon / 10000000, 6));
                }
                else {
                    display.drawString (128, 24, "LA " + String((float)peers[i].gpsrec.lat / 10000000, 6));
                    display.drawString (128, 34, "LO "+ String((float)peers[i].gpsrec.lon / 10000000, 6));
                }

                display.setTextAlignment (TEXT_ALIGN_LEFT);

                if (iscurrent) {
                    display.drawString (0, 24, "A " + String(curr.gps.alt) + "m");
                    display.drawString (0, 34, "S " + String(peers[i].gpsrec.groundSpeed / 100) + "m/s");
                    display.drawString (0, 44, "C " + String(curr.gps.groundCourse / 10) + "°");
                }
                else {
                    display.drawString (0, 24, "A " + String(peers[i].gpsrec.alt) + "m");
                    display.drawString (0, 34, "S " + String(peers[i].gpsrec.groundSpeed / 100) + "m/s");
                    display.drawString (0, 44, "C " + String(peers[i].gpsrec.groundCourse / 10) + "°");
                }

                if (peers[i].gps.lat != 0 && peers[i].gps.lon != 0 && curr.gps.lat != 0 && curr.gps.lon != 0 && !iscurrent) {


                    double lat1 = curr.gps.lat / 10000000;
                    double lon1 = curr.gps.lon / 10000000;
                    double lat2 = peers[i].gpsrec.lat / 10000000;
                    double lon2 = peers[i].gpsrec.lon / 10000000;

                    peers[i].distance = gpsDistanceBetween(lat1, lon1, lat2, lon2);
                    peers[i].direction = gpsCourseTo(lat1, lon1, lat2, lon2);
                    peers[i].relalt = peers[i].gpsrec.alt - curr.gps.alt;

                    display.drawString (40, 44, "B " + String(peers[i].direction) + "°");
                    display.drawString (88, 44, "D " + String(peers[i].distance) + "m");
                    display.drawString (0, 54, "R " + String(peers[i].relalt) + "m");
                }

                if (iscurrent) {
                    display.drawString (40, 54, String((float)curr.fcanalog.vbat / 10) + "v");
                    display.drawString (88, 54, String((int)curr.fcanalog.mAhDrawn) + "mah");
                }
                else {
                    display.drawString (40, 54, String((float)peers[i].fcanalog.vbat / 10) + "v");
                    display.drawString (88, 54, String((int)peers[i].fcanalog.mAhDrawn) + "mah");
                }

            display.setTextAlignment (TEXT_ALIGN_RIGHT);

        }
        else {
            display.drawString (35, 7, "SLOT IS EMPTY");
            sys.display_page++;
        }

    }

    display.display();
}

void display_logo() {
    display.drawXbm(0, 0, LOGO_WIDTH, LOGO_HEIGHT, logo_bits_s);
    display.display();
    delay(1200);
    display.clear();
}

// -------- MSP and FC

void msp_get_state() {
    uint32_t planeModes;
    msp.getActiveModes(&planeModes);
    curr.state = bitRead(planeModes, 0);
}

void msp_get_name() {
    msp.request(MSP_NAME, &curr.name, sizeof(curr.name));
    curr.name[6] = '\0';
}

void msp_get_gps() {
    msp.request(MSP_RAW_GPS, &curr.gps, sizeof(curr.gps));
}

void msp_set_fc() {
    char j[5];
    curr.host = HOST_NONE;
    msp.request(MSP_FC_VARIANT, &j, sizeof(j));

    if (strncmp(j, "INAV", 4) == 0) {
        curr.host = HOST_INAV;
    }
    else if (strncmp(j, "BTFL", 4) == 0) {
        curr.host = HOST_BTFL;
    }

    if (curr.host == HOST_INAV || curr.host == HOST_BTFL) {
        msp.request(MSP_FC_VERSION, &curr.fcversion, sizeof(curr.fcversion));
    }
 }

 void msp_get_fcanalog() {
  msp.request(MSP_ANALOG, &curr.fcanalog, sizeof(curr.fcanalog));
}

void msp_send_radar(uint8_t i) {
    radarPos.id = i;
    radarPos.state = peers[i].state;
    radarPos.lat = peers[i].gps.lat; // x 10E7
    radarPos.lon = peers[i].gps.lon; // x 10E7
    radarPos.alt = peers[i].gps.alt * 100; // cm
    radarPos.heading = peers[i].gps.groundCourse / 10; // From ° x 10 to °
    radarPos.speed = peers[i].gps.groundSpeed; // cm/s
    radarPos.lq = peers[i].lq;
    msp.command2(MSP2_COMMON_SET_RADAR_POS , &radarPos, sizeof(radarPos), 0);
//    msp.command(MSP_SET_RADAR_POS , &radarPos, sizeof(radarPos), 0);
}

void msp_send_peers() {
    for (int i = 0; i < cfg.lora_nodes_max; i++) {
        if (peers[i].id > 0) {
            msp_send_radar(i);
        }
    }
}

void msp_send_peer(uint8_t peer_id) {
    if (peers[peer_id].id > 0) {
        msp_send_radar(peer_id);
    }
}

// -------- INTERRUPTS

const byte interruptPin = 0;
volatile int interruptCounter = 0;
int numberOfInterrupts = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR handleInterrupt() {
    portENTER_CRITICAL_ISR(&mux);

    if (sys.io_button_pressed == 0) {
        sys.io_button_pressed = 1;

        if (sys.phase > MODE_LORA_SYNC) {
            sys.display_page++;
        }
        else if (sys.phase == MODE_MENU) {
            sys.menu_line++;
            sys.menu_begin = millis();
            display.clear();
            if (sys.menu_line > 3) {
            sys.menu_line = 0;
            }
        }
        sys.io_button_released = millis();
    }
    portEXIT_CRITICAL_ISR(&mux);
}


// ----------------------------- setup

void setup() {

    #ifdef USE_CLI
    Serial.begin(115200);
    cli->begin(Serial);
    #endif

    #ifdef USE_BT
    SerialBT.begin("INAV-Radar");
    cliBT->begin(SerialBT);
    //SerialBT.end();
    #endif

    sys.phase = MODE_START;

    config_init();

    sys.lora_cycle = cfg.lora_nodes_max * cfg.lora_slot_spacing;
    sys.cycle_stats = sys.lora_cycle * 2;

    pinMode(cfg.io_pin_led, OUTPUT);
    sys.io_led_blink = 0;

    if (cfg.display_enable) {
        display_init();
        display_logo();
        display.clear();
        display.display();
    }

    msp.begin(Serial1);
    Serial1.begin(115200, SERIAL_8N1, SERIAL_PIN_RX , SERIAL_PIN_TX);
    reset_peers();

    pinMode(interruptPin, INPUT);
    sys.io_button_pressed = 0;
    attachInterrupt(digitalPinToInterrupt(interruptPin), handleInterrupt, RISING);


/*
    sys.display_updated = 0;
    sys.menu_line = CFG_PROFILE_DEFAULT;
    sys.menu_line = 1; // ---------------------
    sys.menu_begin = millis();
    sys.phase = MODE_MENU;

    */

    lora_init();

    if (cfg.display_enable) {
        display.clear();
        display.drawString(0, 0, "RADAR VERSION " + String(VERSION));
        display.drawString(0, 9, "PROFILE " + String(cfg.profile_id) + " " + String(cfg.profile_name));
        display.drawString(0, 18, "HOST");
        display.display();
    }

    sys.cycle_scan_begin = millis();
    sys.now = millis();

    curr.host = HOST_NONE;
    sys.phase = MODE_HOST_SCAN;

}

// ----------------------------------------------------------------------------- MAIN LOOP

void loop() {

    #ifdef USE_CLI
    cli->process();
    #endif

    #ifdef USE_BT
    cliBT->process();
    #endif

    sys.now = millis();

// ---------------------- IO BUTTON

    if ((sys.now > sys.io_button_released + 150) && (sys.io_button_pressed == 1)) {
        sys.io_button_pressed = 0;
    }


// ---------------------- HOST SCAN

    if (sys.phase == MODE_HOST_SCAN) {

        if ((sys.now > (sys.cycle_scan_begin + HOST_MSP_TIMEOUT)) || (curr.host != HOST_NONE)) {  // End of the host scan

            if (curr.host != HOST_NONE) {
                msp_get_name();
            }

            if (curr.name[0] == '\0') {
                for (int i = 0; i < 4; i++) {
                curr.name[i] = (char) random(65, 90);
                curr.name[4] = 0;
                }
            }

            curr.gps.fixType = 0;
            curr.gps.lat = 0;
            curr.gps.lon = 0;
            curr.gps.alt = 0;
            curr.id = 0;

            LoRa.sleep();
            LoRa.receive();

        if (cfg.display_enable) {
            if (curr.host > 0) {
                display.drawString (35, 18, String(host_name[curr.host]) + " " + String(curr.fcversion.versionMajor) + "."  + String(curr.fcversion.versionMinor) + "." + String(curr.fcversion.versionPatchLevel));
            }
            else {
                display.drawString (35, 18, String(host_name[curr.host]));
            }

            display.drawProgressBar(0, 53, 40, 6, 100);
            display.drawString (0, 27, "SCAN");
            display.display();

        }

            sys.cycle_scan_begin = millis();
            sys.phase = MODE_LORA_INIT;

        } else { // Still scanning
            if (sys.now > sys.display_updated + DISPLAY_CYCLE / 2) {

                delay(50);

                msp_set_fc();

        if (cfg.display_enable) {
                display.drawProgressBar(0, 53, 40, 6, 100 * (millis() - sys.cycle_scan_begin) / HOST_MSP_TIMEOUT);
                display.display();
        }
                sys.display_updated = millis();
            }
        }
    }

// ---------------------- LORA INIT

    if (sys.phase == MODE_LORA_INIT) {

        if (sys.now > (sys.cycle_scan_begin + LORA_CYCLE_SCAN)) {  // End of the scan, set the ID then sync

            sys.num_peers = count_peers();

            if (sys.num_peers >= cfg.lora_nodes_max) {
                sys.lora_no_tx = 1;
            }
            else {
                pick_id();
            }
            sys.display_page = 0;
            sys.phase = MODE_LORA_SYNC;

        } else { // Still scanning

            if (sys.now > sys.display_updated + DISPLAY_CYCLE / 2 && cfg.display_enable) {
                for (int i = 0; i < cfg.lora_nodes_max; i++) {
                    if (peers[i].id > 0) {
                        display.drawString(40 + peers[i].id * 8, 27, String(peer_slotname[peers[i].id]));
                    }
                }
                display.drawProgressBar(40, 53, 86, 6, 100 * (millis() - sys.cycle_scan_begin) / LORA_CYCLE_SCAN);
                display.display();

                sys.display_updated = millis();
            }
        delay(20);
        }
    }

// ---------------------- LORA SYNC

    if (sys.phase == MODE_LORA_SYNC) {

        if (sys.num_peers == 0 || sys.lora_no_tx) { // Alone or no_tx mode, start at will
            sys.lora_next_tx = millis() + sys.lora_cycle;
            }
        else { // Not alone, sync by slot
            resync_tx_slot(cfg.lora_timing_delay);
        }
        sys.display_updated = sys.lora_next_tx + sys.lora_cycle - 30;
        sys.stats_updated = sys.lora_next_tx + sys.lora_cycle - 15;

        sys.pps = 0;
        sys.ppsc = 0;
        sys.num_peers = 0;
        stats.packets_total = 0;
        stats.packets_received = 0;
        stats.percent_received = 0;
        digitalWrite(cfg.io_pin_led, LOW);

        sys.phase = MODE_LORA_RX;
        }

// ---------------------- LORA RX

    if ((sys.phase == MODE_LORA_RX) && (sys.now > sys.lora_next_tx)) {

        while (sys.now > sys.lora_next_tx) { // In  case we skipped some beats
            sys.lora_next_tx += sys.lora_cycle;
        }

        if (sys.lora_no_tx) {
            sprintf(sys.message, "%s", "SILENT MODE (NO TX)");
        }
        else {
            if (sys.num_peers == 0) {
                sys.lora_next_tx += random(0, 2) * cfg.lora_slot_spacing;
                }
            sys.phase = MODE_LORA_TX;
        }
    sys.lora_tick++;
    }

// ---------------------- LORA TX

    if (sys.phase == MODE_LORA_TX) {

        if ((curr.host == HOST_NONE) || (curr.gps.fixType < 1)) {
            curr.gps.lat = 0;
            curr.gps.lon = 0;
            curr.gps.alt = 0;
            curr.gps.groundCourse = 0;
            curr.gps.groundSpeed = 0;
        }

        sys.lora_last_tx = millis();
        lora_send();
        stats.last_tx_duration = millis() - sys.lora_last_tx;

        // Drift correction

        if (curr.id > 1) {
            int prev = curr.id - 2;
            if (peers[prev].id > 0) {
                sys.lora_drift = sys.lora_last_tx - peers[prev].updated - cfg.lora_slot_spacing;

                if ((abs(sys.lora_drift) > LORA_DRIFT_THRESHOLD) && (abs(sys.lora_drift) < cfg.lora_slot_spacing)) {
                    sys.drift_correction = constrain(sys.lora_drift, -LORA_DRIFT_CORRECTION, LORA_DRIFT_CORRECTION);
                    sys.lora_next_tx -= sys.drift_correction;
                    sprintf(sys.message, "%s %3d", "TIMING ADJUST", -sys.drift_correction);
                }
            }
        }

        sys.lora_slot = 0;
        sys.msp_next_cycle = sys.lora_last_tx + cfg.msp_after_tx_delay;

        // Back to RX

        LoRa.sleep();
        LoRa.receive();

        sys.phase = MODE_LORA_RX;
    }

// ---------------------- DISPLAY

    if ((sys.now > sys.display_updated + DISPLAY_CYCLE) && sys.display_enable && (sys.phase > MODE_LORA_SYNC) && cfg.display_enable) {

        stats.timer_begin = millis();

        if (sys.num_peers == 0 && sys.display_page == 1)  { // No need for timings graphs when alone
            sys.display_page++;
        }

        if (sys.display_page >= (3 + cfg.lora_nodes_max)) {
            sys.display_page = 0;
        }

        display_draw();

        sys.message[0] = 0;

        stats.last_oled_duration = millis() - stats.timer_begin;
        sys.display_updated = sys.now;
    }

// ---------------------- SERIAL / MSP

    if (sys.now > sys.msp_next_cycle && curr.host != HOST_NONE && sys.phase > MODE_LORA_SYNC && sys.lora_slot < cfg.lora_nodes_max) {

        stats.timer_begin = millis();

        if (sys.lora_slot == 0) {

            if (sys.lora_tick % 6 == 0) {
                msp_get_state();
            }

            if ((sys.lora_tick + 1) % 6 == 0) {
                msp_get_fcanalog();
            }

        }

        msp_get_gps(); // GPS > FC > ESP
        msp_send_peer(sys.lora_slot); // ESP > FC > OSD

        stats.last_msp_duration[sys.lora_slot] = millis() - stats.timer_begin;
        sys.msp_next_cycle += cfg.lora_slot_spacing;
        sys.lora_slot++;
    }


// ---------------------- STATISTICS & IO

    if ((sys.now > (sys.cycle_stats + sys.stats_updated)) && (sys.phase > MODE_LORA_SYNC)) {

        sys.pps = sys.ppsc;
        sys.ppsc = 0;

        // Timed-out peers + LQ

        for (int i = 0; i < cfg.lora_nodes_max; i++) {

            if (sys.now > (peers[i].lq_updated +  sys.lora_cycle * 4)) {
                uint16_t diff = peers[i].updated - peers[i].lq_updated;
                peers[i].lq = constrain(peers[i].lq_tick * 4.2 * sys.lora_cycle / diff, 0, 4);
                peers[i].lq_updated = sys.now;
                peers[i].lq_tick = 0;
            }

            if (peers[i].id > 0 && ((sys.now - peers[i].updated) > LORA_PEER_TIMEOUT)) {
                peers[i].lost = 1;

                if ((sys.now - peers[i].updated) > LORA_PEER_TIMEOUT_LOST) {
                    peers[i].state = 2;
                }
            }
        }

        sys.num_peers_active = count_peers(1);

//        sys.lora_slow_mode = (sys.num_peers_active == 0) ? 1 : 0;

        stats.packets_total += sys.num_peers_active * sys.cycle_stats / sys.lora_cycle;
        stats.packets_received += sys.pps;
        stats.percent_received = (stats.packets_received > 0) ? constrain(100 * stats.packets_received / stats.packets_total, 0 ,100) : 0;

        // Screen management

        if (!curr.state && !sys.display_enable) { // Aircraft is disarmed = Turning on the OLED
            display.displayOn();
            sys.display_enable = 1;
        }

        else if (curr.state && sys.display_enable) { // Aircraft is armed = Turning off the OLED
            display.displayOff();
            sys.display_enable = 0;
        }

    sys.stats_updated = sys.now;
    }


    // LED blinker

    if (sys.lora_tick % 6 == 0) {
        if (sys.num_peers_active > 0) {
            sys.io_led_changestate = millis() + IO_LEDBLINK_DURATION;
            sys.io_led_count = 0;
            sys.io_led_blink = 1;
        }
    }

    if (sys.io_led_blink && millis() > sys.io_led_changestate) {

        sys.io_led_count++;
        sys.io_led_changestate += IO_LEDBLINK_DURATION;

        if (sys.io_led_count % 2 == 0) {
            digitalWrite(cfg.io_pin_led, LOW);
        }
        else {
            digitalWrite(cfg.io_pin_led, HIGH);
        }

        if (sys.io_led_count >= sys.num_peers_active * 2) {
            sys.io_led_blink = 0;
        }

    }

}
