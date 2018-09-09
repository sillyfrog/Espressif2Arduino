#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

extern "C" uint8_t system_upgrade_userbin_check();
extern "C" void system_upgrade_flag_set(uint8 flag);
extern "C" void system_upgrade_reboot(void);
extern "C" uint8_t system_upgrade_flag_check();

#define AP_SSID "FinalStage"

#define MAGIC_V1 0xE9
#define MAGIC_V2 0xEA
#define UPGRADE_FLAG_START 0x01
#define UPGRADE_FLAG_FINISH 0x02
#define SECTOR_SIZE 4096
#define BUFFER_SIZE 1024
#define TIMEOUT 5000

byte buffer[BUFFER_SIZE] __attribute__((aligned(4))) = {0};
byte bootrom[SECTOR_SIZE] __attribute__((aligned(4))) = {0};
byte _blink = 0;

enum FlashMode
{
  MODE_UNKNOWN,
  MODE_FLASH_ROM1,
  MODE_FLASH_ROM2
};

//USER EDITABLE VARABLES HERE
#define DELAY 100      //ms to blink/delay for
#define STATUS_GPIO 13 //gpio to toggle as status indicator
#define RETRY 3        //number of times to retry

#define URL_ROM_2 "http://192.168.4.2:8080/ota/image_user2-0x81000.bin"
#define URL_ROM_3 "http://192.168.4.2:8080/ota/image_arduino.bin"

//Uncomment to provide fixed credentials - otherwise will try to use credentials saved by sonoff device
//#define WIFI_SSID "TEST"
//#define WIFI_PASSWORD "PASSWORD"

void setup()
{
  Serial.begin(115200);
  Serial.print("\nInitalizing...");
  if (STATUS_GPIO)
  {
    pinMode(STATUS_GPIO, OUTPUT);
  }

  //blink our status LED while we wait for serial to come up
  for (int i = 0; i < 100; i++)
  {
    blink();
    delay(DELAY);
  }
  digitalWrite(STATUS_GPIO, HIGH);
  Serial.println("Done");

  uint8_t upgrade = determineUpgradeRom();
  if (upgrade == MODE_FLASH_ROM1 || MODE_FLASH_ROM2)
  {
    connectToWiFiBlocking();
    digitalWrite(STATUS_GPIO, LOW);
  }

  Serial.print("Flash Mode: ");
  FlashMode_t flashmode = ESP.getFlashChipMode();
  Serial.println(flashmode);

  if (upgrade == MODE_FLASH_ROM1)
    flashRom1();
  else if (upgrade == MODE_FLASH_ROM2)
    flashRom2();
  else
    Serial.println("ROM or Flash Mode not recognized");
}

uint8_t determineUpgradeRom()
{
  Serial.printf("Current rom: ");
  uint8_t rom = system_upgrade_userbin_check() + 1;
  Serial.printf("%d\n", rom);

  Serial.printf("Rom 1 magic byte: ");
  uint32_t rom_1_start_address = 0x001000;
  byte magic = 0;
  ESP.flashRead(rom_1_start_address, (uint32_t *)&magic, 1);
  Serial.printf("0x%02X\n", magic);

  uint8_t mode = MODE_UNKNOWN;
  if (magic == MAGIC_V2)
  {
    if (rom == 1)
      mode = MODE_FLASH_ROM2;
    else if (rom == 2)
      mode = MODE_FLASH_ROM1;
  }

  // Not sure how this state happens, but last ditch effort to still flash
  if (magic == MAGIC_V1 && rom == 2)
  {
    mode = MODE_FLASH_ROM1;
    Serial.printf("Magic is V1, but on ROM 2, try to flash ROM 1\n");
  }

  Serial.printf("Reflashing rom: %d\n", mode);
  Serial.printf("Upgrade ROM mode: ");
  if (mode == MODE_FLASH_ROM1)
    Serial.printf("MODE_FLASH_ROM1\n");
  else if (mode == MODE_FLASH_ROM2)
    Serial.printf("MODE_FLASH_ROM2\n");
  else if (mode == MODE_UNKNOWN)
    Serial.printf("MODE_UNKNOWN\n");
  else
    Serial.printf("ERROR - this should not happen\n");

  return mode;
}

void connectToWiFiBlocking()
{
  //char ssid[32] = {0};
  //char pass[64] = {0};

  //Serial.print("Attemping to read Sonoff Wifi credentials... ");
  //ESP.flashRead(0x79000, (uint32_t *) &ssid[0], sizeof(ssid));
  //ESP.flashRead(0x79020, (uint32_t *) &pass[0], sizeof(pass));
  //Serial.print("Done\n");
  //Serial.printf("\tSSID: %s\n", ssid);
  //Serial.printf("\tPassword: %s\n", pass);

  Serial.printf("Configuring Wifi %s...\n", AP_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(AP_SSID);

  WiFiClient client;
  bool connected = false;

  // Wait for the PC to connect to the WiFi and start the web server
  while (!connected)
  {
    delay(500);
    Serial.println("Waiting for connection...");
    blink();
    client.connect("192.168.4.2", 8080);
    if (client.connected())
    {
      Serial.println("Connected");
      connected = true;
    }
  }
  Serial.println("Web server accessible...");
  //IPAddress ip = WiFi.localIP();
  //Serial.printf("\t%d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
}

void flashRom1()
{
  bool result = downloadRomToFlash(
      1,       //Rom 1
      true,    //Bootloader is being updated
      0xE9,    //Standard Arduino Magic
      0x00000, //Write to 0x0 since we are replacing the bootloader
      0x80000, //Stop before 0x80000
      0,       //Erase Sector from 0 to
      128,     //Sector 128 (not inclusive)
      URL_ROM_3,
      RETRY //Retry Count
  );

  ESP.restart(); //restart regardless of success
}

//Download special rom.
void flashRom2()
{
  system_upgrade_flag_set(UPGRADE_FLAG_START);
  bool result = downloadRomToFlash(
      2,        //Rom 2
      false,    //Bootloader is not being updated
      0xEA,     //V2 Espressif Magic
      0x081000, //Not replacing bootloader
      0x100000, //Stop before end of ram
      128,      //From middle of flash
      256,      //End of flash
      URL_ROM_2,
      RETRY //Retry Count
  );

  if (result)
  {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
    Serial.print("system_upgrade_flag_check(): ");
    Serial.println(system_upgrade_flag_check());
    system_upgrade_reboot();
  }
  else
  {
    ESP.restart();
  }
}

//Assumes bootloader must be in first SECTOR_SIZE bytes.
bool downloadRomToFlash(byte rom, byte bootloader, byte magic,
                        uint32_t start_address, uint32_t end_address, uint16_t erase_sectior_start,
                        uint16_t erase_sector_end, const char *url, uint8_t retry_limit)
{
  uint8_t retry_counter = 0;
  while (retry_counter < retry_limit)
  {
    uint16_t erase_start = erase_sectior_start;
    uint32_t write_address = start_address;
    uint8_t header[4] = {0};
    bootrom[SECTOR_SIZE] = {0};
    buffer[BUFFER_SIZE] = {0};

    Serial.printf("Flashing rom %d (retry:%d): %s\n", rom, retry_counter, url);
    HTTPClient http;
    http.begin(url);
    http.useHTTP10(true);
    http.setTimeout(TIMEOUT);

    //Response Code Check
    uint16_t httpCode = http.GET();
    Serial.printf("HTTP response Code: %d\n", httpCode);
    if (httpCode != HTTP_CODE_OK)
    {
      Serial.println("Invalid response Code - retry");
      retry_counter++;
      continue;
    }

    //Length Check (at least one sector)
    uint32_t len = http.getSize();
    Serial.printf("HTTP response length: %d\n", len);
    if (len < SECTOR_SIZE)
    {
      Serial.println("Length too short - retry");
      retry_counter++;
      continue;
    }

    if (len > (end_address - start_address))
    {
      Serial.println("Length exceeds flash size - retry");
      retry_counter++;
      continue;
    }

    //Confirm magic byte
    WiFiClient *stream = http.getStreamPtr();
    stream->peekBytes(&header[0], 4);
    Serial.printf("Magic byte from stream: 0x%02X\n", header[0]);
    if (header[0] != magic)
    {
      Serial.println("Invalid magic byte - retry");
      retry_counter++;
      continue;
    }

    if (bootloader)
    {
      Serial.printf("Downloading %d byte bootloader", sizeof(bootrom));
      size_t size = stream->available();
      while (size < sizeof(bootrom))
      {
        blink();
        Serial.print("Current size: ");
        Serial.print(size);
        Serial.print(" sizeof(bootrom): ");
        Serial.println(sizeof(bootrom));
        size = stream->available();
      }
      int c = stream->readBytes(bootrom, sizeof(bootrom));

      //Skip the bootloader section for the moment..
      erase_start++;
      write_address += SECTOR_SIZE;
      len -= SECTOR_SIZE;
      Serial.printf(".Done\n");
    }

    Serial.printf("Erasing flash sectors %d-%d", erase_start, erase_sector_end);
    for (uint16_t i = erase_start; i < erase_sector_end; i++)
    {
      ESP.flashEraseSector(i);
      blink();
    }
    Serial.printf("Done\n");

    Serial.printf("Downloading rom to 0x%06X-0x%06X in %d byte blocks", write_address, write_address + len, sizeof(buffer));
    //Serial.println();
    while (len > 0)
    {
      size_t size = stream->available();
      if (size >= sizeof(buffer) || size == len)
      {
        int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
        //Serial.printf("address=0x%06X, bytes=%d, len=%d\n", write_address, c, len);
        ESP.flashWrite(write_address, (uint32_t *)&buffer[0], c);
        write_address += c; //increment next write address
        len -= c;           //decremeant remaining bytes
      }
      blink();
      //delay(100);
    }
    http.end();
    Serial.println("Done");

    if (bootloader)
    {
      Serial.printf("Erasing bootloader sector 0");
      ESP.flashEraseSector(0);
      Serial.printf("..Done\n");

      Serial.printf("Writing bootloader to 0x%06X-0x%06X", 0, SECTOR_SIZE);
      ESP.flashWrite(0, (uint32_t *)&bootrom[0], SECTOR_SIZE);
      Serial.printf("..Done\n");
    }

    return true;
  }
  Serial.println("Retry counter exceeded - giving up");
  return false;
}

void blink()
{
  if (STATUS_GPIO)
  {
    if (_blink)
      digitalWrite(STATUS_GPIO, LOW);
    else
      digitalWrite(STATUS_GPIO, HIGH);

    _blink ^= 0xFF;
  }
  Serial.print(".");
  yield(); // reset watchdog
}

void loop()
{
  delay(100);
}
