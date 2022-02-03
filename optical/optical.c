/**
Author: Paul Bupe Jr

Based on the i2c port of https://github.com/74ls04/VL53L3CX_rasppi
*/

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <vl53lx_api.h>
#include "vl53lx_platform.h"
#include <czmq.h>
#include <assert.h>

// #define XSHUTPIN 4 // GPIO4

// Requires zeromq library for publishing sensor data over the network. 
//    apt-get install libczmq-dev

VL53LX_Dev_t dev;
VL53LX_DEV Dev = &dev;
int status;

enum hist_mode {
    HIST_A,
    HIST_B,
    HIST_BOTH,
};

// Command line options
bool hist_flag = false; // Flag to enable histogram mode
enum hist_mode hist_mode = HIST_B; // [-g] Histogram mode. A, B, or AB for both. (default: B)
bool formatted_flag = false; // [-f] Enable debug messages
int poll_period = 100; // [-p] Device polling period in (ms)
int timing_budget = 33; // [-t] VL53L3CX timing budget (8ms to 500ms)
int XSHUTPIN = 4; // [-x] GPIO pin for XSHUT (default: 4)
uint8_t address = 0x29; // [-a] VL53L3CX I2C address (Default is 0x29)
const char *argv0;

void RangingLoop(void); 
void ctrl_c_handler(int signal);

static void help(void)
{
  printf("\n");
  printf("This program continuously reads from the VL53L3CX sensor and optionally\n");
  printf("provides the A, B or both histogram bin data.\n");
  printf("\n");
	printf("Usage: %s [options]\n", argv0);
	printf("\n");
  printf("Options:\n");
  printf("  -a [address]\n");
  printf("    Desired VL53L3CX I2C HEX address (default is 0x29).\n\n");  
  printf("  -x [pin]\n");
  printf("    GPIO pin for XSHUT, NOT the header physical pin number\n");
  printf("    (default is 4, also called GPIO4).\n\n");
  printf("  -p [ms]>\n");
  printf("    Device polling period in ms (default is 100 ms).\n\n");
  printf("  -t [ms]\n");
  printf("    VL53L3CX timing budget [8ms - 500ms] (default is 33 ms).\n\n");
  printf("  -g [A, B, AB]\n");
  printf("    Enable the A, B, or both histograms.\n\n");
  printf("  -f\n");
  printf("    Print formatted readings.\n");
  printf("\n");
}

int main(int argc, char *argv[])
{

  int opt;
  char hist_value[128];
  argv0 = argv[0];

  // Print help if no arguments
  if (argc == 1) {
    help();
    exit(EXIT_SUCCESS);
  }

  while ((opt = getopt(argc, argv, "a:dg:hp:t:x:")) != -1) {
    switch (opt) {
      case 'a': // i2c address
        address = strtol(optarg, NULL, 16);
        break;
      case 'g': // Read histogram mode
        strncpy(hist_value, optarg, sizeof(hist_value));
        if (strcmp(hist_value, "A") == 0) {
          hist_mode = HIST_A;
        } else if (strcmp(hist_value, "B") == 0) {
          hist_mode = HIST_B;
        } else if (strcmp(hist_value, "AB") == 0) {
          hist_mode = HIST_BOTH;
        } else {
          printf("Invalid histogram mode: %s\n", hist_value);
          exit(EXIT_FAILURE);
        }
        hist_flag = true;
        break;
      case 'f': // Enable formatted debug messages
        formatted_flag = true;
        break;
      case 'p': // Device polling period
        poll_period = atoi(optarg);
        break;
      case 't': // VL53L3CX timing budget
        timing_budget = atoi(optarg);
        break;
      case 'x': // XSHUT GPIO pin
        XSHUTPIN = atoi(optarg);
        break;
      case '?': 
      case 'h':
        help();
        exit(EXIT_SUCCESS);
      default:
        help();
        exit(EXIT_FAILURE);
    }
  }

  // Register signal handler
  signal(SIGINT, ctrl_c_handler);

  // Turn on the sensor using GPIO4
  // Enable GPIO4 using sysfs
  char buf[100];

  FILE *fp = fopen("/sys/class/gpio/export", "w");
  if (fp == NULL)
  {
    printf("Failed to open /sys/class/gpio/export\n");
    raise(SIGTERM);
  }
  fprintf(fp, "%d", XSHUTPIN);
  fclose(fp);

  // Give the udev rules a chance to make the GPIO available
  sleep(1);
  
  // Set GPIO4 as output
  sprintf(buf, "/sys/class/gpio/gpio%d/direction", XSHUTPIN);
  fp = fopen(buf, "w");
  if (fp == NULL)
  {
    printf("Failed to open %s\n", buf);
    raise(SIGTERM);
  }
  fprintf(fp, "out");
  fclose(fp);

  // Set GPIO4 high
  sprintf(buf, "/sys/class/gpio/gpio%d/value", XSHUTPIN);
  fp = fopen(buf, "w");
  if (fp == NULL)
  {
    printf("Failed to open %s\n", buf);
    raise(SIGTERM);
  }
  fprintf(fp, "1");
  fclose(fp);

  // Delay for a bit
  usleep(10000); // 10 millisecond 

  Dev->i2c_slave_address = 0x29;
  Dev->fd = VL53LX_i2c_init("/dev/i2c-1", Dev->i2c_slave_address); //choose between i2c-0 and i2c-1; On the raspberry pi zero, i2c-1 are pins 2 and 3
  if (Dev->fd < 0)
  {
    printf("Failed to init 4\n");
    raise(SIGTERM);
  }

  uint8_t byteData;
  uint16_t wordData;
  VL53LX_RdByte(Dev, 0x010F, &byteData);
  printf("VL53L3cX Model_ID: %02X\n\r", byteData);
  if (byteData != 0xea)
  {
    printf("WARNING: Model Id is not 0xea, which it ought to be!\n");
    raise(SIGTERM);
  }
  
  RangingLoop();

}

// CTRL-C handler
void ctrl_c_handler(int signal)
{
  printf("\n\rExiting...\n\r");

  // Turn off the sensor using GPIO4
  char buf[100];
  sprintf(buf, "/sys/class/gpio/gpio%d/value", XSHUTPIN);
  FILE *fp = fopen(buf, "w");
  if (fp == NULL)
  {
    printf("Failed to open %s\n", buf);
    return;
  }
  fprintf(fp, "0");
  fclose(fp);

  // Disable GPIO4 using sysfs
  fp = fopen("/sys/class/gpio/unexport", "w");
  if (fp == NULL)
  {
    printf("Failed to open /sys/class/gpio/unexport\n");
    // return;
  }
  fprintf(fp, "%d", XSHUTPIN);
  fclose(fp);

  exit(signal);
}

// Ranging loop
void RangingLoop(void)
{
  //  Socket to talk to clients
  void *context = zmq_ctx_new();
  void *publisher = zmq_socket(context, ZMQ_PUB);
  int rc = zmq_bind(publisher, "tcp://*:5556");
  assert(rc == 0);

  VL53LX_MultiRangingData_t MultiRangingData;
  VL53LX_MultiRangingData_t *pMultiRangingData = &MultiRangingData;
  VL53LX_Error status2;
  VL53LX_Error status3;
  VL53LX_AdditionalData_t AdditionalData;
  VL53LX_AdditionalData_t *pAdditionalData = &AdditionalData;

  uint8_t NewDataReady = 0;
  int no_of_object_found = 0;
  int j;
  char tmp_data1[5], tmp_data2[512], data[3000];
  char histogram_data_buffer[500];
  char bin_buffer[5];

  printf("Ranging loop starts...\n");

  status = VL53LX_WaitDeviceBooted(Dev);
  status = VL53LX_DataInit(Dev);

  if (status)
  {
    printf("VL53LX_StartMeasurement failed: error = %d \n", status);
    raise(SIGTERM);
  }

  status = VL53LX_SetMeasurementTimingBudgetMicroSeconds(Dev, timing_budget * 1000);
  status = VL53LX_StartMeasurement(Dev);

  do
  { // polling mode
    VL53LX_DistanceModes *distance_mode = malloc(sizeof(VL53LX_DistanceModes));
    status2 = VL53LX_GetDistanceMode(Dev, distance_mode);
    status = VL53LX_GetMeasurementDataReady(Dev, &NewDataReady);

    usleep(poll_period * 1000); // Polling period

    if ((!status) && (NewDataReady != 0))
    {

      status = VL53LX_GetMultiRangingData(Dev, pMultiRangingData);

      /*
    From https://community.st.com/s/question/0D53W00000etcEZ/understanding-vl53l3cx-histogram-data

    We use:
    VL53LX_Error VL53LX_GetAdditionalData(VL53LX_DEV Dev,
    VL53LX_AdditionalData_t *pAdditionalData)
    This function will return the histogram data.
    But there is a trick. The data is formatted for the hardware and not for you.
    Under the covers there are 2 ranges (an 'a' and a 'b' range)
    The first 2 'bins' of the arrays are NOT histogram data.
    And the next 4 bins of the 'a' array are ambient data - not histogram data.
    So if you use VL53LX_GetAdditionalData and plot starting at bin 6 of the odd ranges and bin 2 of the even ranges you will do better.
    The A ranges have 20 valid bins, the B ranges have 24 valid ones.

    From https://community.st.com/s/question/0D53W00001Gl6B2SAJ/can-someone-please-post-sample-code-on-how-to-get-histogram-data-from-vl53l3cx-i-have-the-dev-board-nucleo-f401-re-and-the-both-the-sensor-and-the-eval-kit

    But it's not quite that easy. There are 2 ranges and they toggle back an forth. One range consists of 4 bins of ambient light and 20 data bins.
    The alternating range consists of 24 range bins.
    Range on a flat wall print out the 24 bins, gathering the data into a spreadsheet.
    You will soon figure it out.
    (The two range timing have to do with a search for 'radar aliasing' effects. google it.)
    And each bin is about 20cm worth of distance. 

    */

      no_of_object_found = pMultiRangingData->NumberOfObjectsFound;
            // zmq topic
      strcat(data, "sensor1 ");

      sprintf(tmp_data1, "%d ", pMultiRangingData->StreamCount);
      strcat(data, tmp_data1);
      memset(tmp_data1, 0, sizeof(tmp_data1));

      // Process if object is found
      if (no_of_object_found > 0) {


      if (hist_flag) {  
      VL53LX_GetAdditionalData(Dev, pAdditionalData);

      // Convert the histogram data to a comma-separated string


      for (j = 0; j < VL53LX_HISTOGRAM_BUFFER_SIZE; j++)
      {
        if (j == VL53LX_HISTOGRAM_BUFFER_SIZE - 1)
        {
          sprintf(bin_buffer, "%d ", pAdditionalData->VL53LX_p_006.bin_data[j]);
          strcat(histogram_data_buffer, bin_buffer);
        }
        else
        {
          sprintf(bin_buffer, "%d,", pAdditionalData->VL53LX_p_006.bin_data[j]);
          strcat(histogram_data_buffer, bin_buffer);
        }
      }

      /*
      if ((pMultiRangingData->StreamCount & 1) == 1) // odd
      {
          for (j = 1; j < 24; j++)
          {
              printf("%ld,", pAdditionalData->VL53LX_p_006.bin_data[j]);
          }
          printf("\n");
      }
      else
      {
          for (j = 5; j < 24; j++)
          {
              printf("%ld,", pAdditionalData->VL53LX_p_006.bin_data[j]);
          }
          printf("\n");
      } */

      
      // Copy to data buffer
      strcat(data, histogram_data_buffer);
      }
//

      for (j = 0; j < no_of_object_found; j++)
      {
        if (j != 0)
          printf("\n                     ");

        if (j == no_of_object_found - 1)
        {
          sprintf(tmp_data2, "%d,%d,%d,%d,%2.2f,%2.2f,%2.2f",
                  pMultiRangingData->RangeData[j].RangeStatus,
                  pMultiRangingData->RangeData[j].RangeMinMilliMeter,
                  pMultiRangingData->RangeData[j].RangeMilliMeter,
                  pMultiRangingData->RangeData[j].RangeMaxMilliMeter,
                  pMultiRangingData->RangeData[j].SigmaMilliMeter / 65536.0,
                  pMultiRangingData->RangeData[j].SignalRateRtnMegaCps / 65536.0,
                  pMultiRangingData->RangeData[j].AmbientRateRtnMegaCps / 65536.0);
        }
        else
        {
          sprintf(tmp_data2, "%d,%d,%d,%d,%2.2f,%2.2f,%2.2f ",
                  pMultiRangingData->RangeData[j].RangeStatus,
                  pMultiRangingData->RangeData[j].RangeMinMilliMeter,
                  pMultiRangingData->RangeData[j].RangeMilliMeter,
                  pMultiRangingData->RangeData[j].RangeMaxMilliMeter,
                  pMultiRangingData->RangeData[j].SigmaMilliMeter / 65536.0,
                  pMultiRangingData->RangeData[j].SignalRateRtnMegaCps / 65536.0,
                  pMultiRangingData->RangeData[j].AmbientRateRtnMegaCps / 65536.0);
        }

        strcat(data, tmp_data2);
        memset(tmp_data2, 0, sizeof(tmp_data2));
        if (formatted_flag) {
        printf("Status=%d\nMin Distance=%d mm\nDistance=%d mm\nMax distance=%d mm\nSigma=%2.2f mm\nSignal Rate=%2.2f Mcps\nAmbient Rate=%2.2f Mcps\n",
                  pMultiRangingData->RangeData[j].RangeStatus,
                  pMultiRangingData->RangeData[j].RangeMinMilliMeter,
                  pMultiRangingData->RangeData[j].RangeMilliMeter,
                  pMultiRangingData->RangeData[j].RangeMaxMilliMeter,
                  pMultiRangingData->RangeData[j].SigmaMilliMeter / 65536.0,
                  pMultiRangingData->RangeData[j].SignalRateRtnMegaCps / 65536.0,
                  pMultiRangingData->RangeData[j].AmbientRateRtnMegaCps / 65536.0);
        }
      }

      printf("\n%s", data);

      // Send data to zmq publisher
      rc = zmq_send(publisher, data, strlen(data), 0);

      // clear buffers
      if (hist_flag) {
      memset(histogram_data_buffer, 0, sizeof(histogram_data_buffer));
      memset(bin_buffer, 0, sizeof(bin_buffer));
      }

      memset(data, 0, sizeof(data));
      }

      if (status == 0)
      {
        status = VL53LX_ClearInterruptAndStartMeasurement(Dev);
      }
    }
  } while (1);

  zmq_close(publisher);
  zmq_ctx_destroy(context);
}
