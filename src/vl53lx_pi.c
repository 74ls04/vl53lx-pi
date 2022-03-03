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
#include <getopt.h>
#include <stdarg.h>
#include <vl53lx_api.h>
#include "vl53lx_platform.h"
#include <czmq.h>
#include <assert.h>

// Macro to turn printing on or off
#define print(format, ...)                 \
    do                                     \
    {                                      \
        if (!quiet_flag)                   \
        {                                  \
            printf(format, ##__VA_ARGS__); \
        }                                  \
    } while (0)

struct _error_text
{
    int code;
    char *text;
} error_text[] = {
    {0, "ERROR: NONE"},
    {-1, "ERROR: CALIBRATION WARNING"},
    {-2, "ERROR: MIN CLIPPED"},
    {-3, "ERROR: UNDEFINED"},
    {-4, "ERROR: INVALID PARAMS"},
    {-5, "ERROR: NOT SUPPORTED"},
    {-6, "ERROR: RANGE ERROR"},
    {-7, "ERROR: TIME OUT"},
    {-8, "ERROR: MODE NOT SUPPORTED"},
    {-9, "ERROR: BUFFER TOO SMALL"},
    {-10, "ERROR: COMMS BUFFER TOO SMALL"},
    {-11, "ERROR: GPIO NOT EXISTING"},
    {-12, "ERROR: GPIO FUNCTIONALITY NOT SUPPORTED"},
    {-13, "ERROR: CONTROL INTERFACE"},
    {-14, "ERROR: INVALID COMMAND"},
    {-15, "ERROR: DIVISION BY ZERO"},
    {-16, "ERROR: REF SPAD INIT"},
    {-17, "ERROR: GPH SYNC CHECK FAIL"},
    {-18, "ERROR: STREAM COUNT CHECK FAIL"},
    {-19, "ERROR: GPH ID CHECK FAIL"},
    {-20, "ERROR: ZONE STREAM COUNT CHECK FAIL"},
    {-21, "ERROR: ZONE GPH ID CHECK FAIL"},
    {-22, "ERROR: XTALK EXTRACTION NO SAMPLE FAIL"},
    {-23, "ERROR: XTALK EXTRACTION SIGMA LIMIT FAIL"},
    {-24, "ERROR: OFFSET CAL NO SAMPLE FAIL"},
    {-25, "ERROR: OFFSET CAL NO SPADS ENABLED FAIL"},
    {-26, "ERROR: ZONE CAL NO SAMPLE FAIL"},
    {-27, "ERROR: TUNING PARM KEY MISMATCH"},
    {-28, "WARNING: REF SPAD CHAR NOT ENOUGH SPADS"},
    {-29, "WARNING: REF SPAD CHAR RATE TOO HIGH"},
    {-30, "WARNING: REF SPAD CHAR RATE TOO LOW"},
    {-31, "WARNING: OFFSET CAL MISSING SAMPLES"},
    {-32, "WARNING: OFFSET CAL SIGMA TOO HIGH"},
    {-33, "WARNING: OFFSET CAL RATE TOO HIGH"},
    {-34, "WARNING: OFFSET CAL SPAD COUNT TOO LOW"},
    {-35, "WARNING: ZONE CAL MISSING SAMPLES"},
    {-36, "WARNING: ZONE CAL SIGMA TOO HIGH"},
    {-37, "WARNING: ZONE CAL RATE TOO HIGH"},
    {-38, "WARNING: XTALK MISSING SAMPLES"},
    {-39, "WARNING: XTALK NO SAMPLES FOR GRADIENT"},
    {-50, "WARNING: XTALK SIGMA LIMIT FOR GRADIENT"},
    {-41, "ERROR: NOT IMPLEMENTED"},
    {-60, "ERROR: PLATFORM SPECIFIC START"},
};

VL53LX_Dev_t dev;
VL53LX_DEV Dev = &dev;
int status;

enum hist_mode
{
    HIST_A,
    HIST_B,
    HIST_BOTH,
};

// Command line options
int hist_flag = 0;                                               // Flag to enable histogram mode
enum hist_mode hist_mode = HIST_BOTH;                            // [-g] Histogram mode. A, B, or AB for both. (default: B)
int compact_flag = 0;                                            // [-f] Enable debug messages
int quiet_flag = 0;                                              // [-q] Disable debug messages
int tcp_port = 5556;                                             // [-p] TCP port to publish on
int poll_period = 33;                                            // [-p] Device polling period in (ms)
int timing_budget = 33;                                          // [-t] VL53L3CX timing budget (8ms to 500ms)
int XSHUTPIN = 4;                                                // [-x] GPIO pin for XSHUT (default: 4)
uint8_t address = 0x29;                                          // [-a] VL53L3CX I2C address (Default is 0x29)
VL53LX_DistanceModes distance_mode = VL53LX_DISTANCEMODE_MEDIUM; // Distance mode. SHORT, MEDIUM, or LONG. (default: MEDIUM)

// delimiter for publishing data
char delimiter = ' ';
const char *argv0;

// Long options
static struct option long_options[] = {
    {"histogram", required_argument, NULL, 'g'},
    {"compact", no_argument, NULL, 'c'},
    {"help", no_argument, NULL, 'h'},
    {"quiet", no_argument, &quiet_flag, 1},
    {"port", required_argument, NULL, 'p'},
    {"distance-mode", required_argument, NULL, 'd'},
    {"poll-period", required_argument, NULL, 'm'},
    {"timing-budget", required_argument, NULL, 't'},
    {"xshut-pin", required_argument, NULL, 'x'},
    {"address", required_argument, NULL, 'a'},
    {NULL, 0, NULL, 0}};

void ranging_loop(void);
void signal_handler(int signal);
void check_status(int status);

static void help(void)
{
    printf("\n");
    printf("Usage: %s [OPTION]...\n", argv0);
    printf("Options:\n");
    printf("  -g, --histogram=NAME\t\t\tShow histogram data. A, B, or AB.\n");
    printf("  -c, --compact\t\t\t\tEnable compact mode.\n");
    printf("  -q, --quiet\t\t\t\tDisable debug messages.\n");
    printf("  -d, --distance-mode=MODE\t\tSet distance mode. SHORT, MEDIUM, or LONG.\n");
    printf("  -p, --port=NUMBER\t\t\tSet the port number for publishing data. Default 5556.\n");
    printf("  -m, --poll-period=MILLISECONDS\tSet device polling period in (ms). Default 33 ms.\n");
    printf("  -t, --timing-budget=MILLISECONDS\tSet VL53L3CX timing budget (8ms to 500ms). Default 33 ms.\n");
    printf("  -x, --xshut-pin=NUMBER\t\tSet GPIO pin for XSHUT.\n");
    printf("  -a, --address=ADDRESS\t\t\tSet VL53L3CX I2C address.\n");
    printf("  -h, --help\t\t\t\tPrint this help message.\n");
    printf("\n");
}

void check_status(int status)
{
    if (status != VL53LX_ERROR_NONE)
    {
        // Loop through error text table
        for (int i = 0; i < sizeof(error_text) / sizeof(error_text[0]); i++)
        {
            if (status == error_text[i].code)
            {
                print("%s\n", error_text[i].text);
            }
        }
    }
}

int main(int argc, char *argv[])
{

    int opt;
    argv0 = argv[0];

    VL53LX_Error status;
    VL53LX_LLDriverData_t *pDev;

    while ((opt = getopt_long(argc, argv, "g:chqd:p:t:m:x:a:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'g':
            hist_flag = 1;
            if (optarg)
            {
                if (strcasecmp(optarg, "A") == 0)
                {
                    hist_mode = HIST_A;
                }
                else if (strcasecmp(optarg, "B") == 0)
                {
                    hist_mode = HIST_B;
                }
                else if (strcasecmp(optarg, "AB") == 0)
                {
                    hist_mode = HIST_BOTH;
                }
                else
                {
                    printf("Invalid histogram mode: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
            }
            break;
        case 'c':
            compact_flag = 1;
            break;
        case 'q':
            quiet_flag = 1;
            break;
        case 'd':
            if (strcasecmp(optarg, "SHORT") == 0)
            {
                distance_mode = VL53LX_DISTANCEMODE_SHORT;
            }
            else if (strcasecmp(optarg, "MEDIUM") == 0)
            {
                distance_mode = VL53LX_DISTANCEMODE_MEDIUM;
            }
            else if (strcasecmp(optarg, "LONG") == 0)
            {
                distance_mode = VL53LX_DISTANCEMODE_LONG;
            }
            else
            {
                printf("Invalid distance mode: %s\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'p':
            tcp_port = atoi(optarg);
            break;
        case 'm':
            poll_period = atoi(optarg);
            break;
        case 't':
            // Check if are is between 8 and 500ms
            if (atoi(optarg) < 8 || atoi(optarg) > 500)
            {
                printf("Invalid timing budget: %s ms. Range [8 - 500ms]\n", optarg);
                exit(EXIT_FAILURE);
            }
            timing_budget = atoi(optarg);
            break;
        case 'x':
            XSHUTPIN = atoi(optarg);
            break;
        case 'a':
            address = (uint8_t)strtol(optarg, NULL, 16);
            break;
        case 'h':
            help();
            exit(EXIT_SUCCESS);
            break;
        case '?':
            // printf("Unknown option %c\n", optopt);
            help();
            exit(EXIT_FAILURE);
            break;
        }
    }

    if (hist_mode == HIST_A)
    {
        printf("Histogram mode: A\n");
    }
    else if (hist_mode == HIST_B)
    {
        print("Histogram mode: B\n");
    }
    else if (hist_mode == HIST_BOTH)
    {
        print("Histogram mode: AB\n");
    }
    // Register signal handler
    signal(SIGINT, signal_handler);

    // Turn on the sensor using GPIO4
    // Enable GPIO4 using sysfs
    char buf[100];

    FILE *fp = fopen("/sys/class/gpio/export", "w");
    if (fp == NULL)
    {
        print("Failed to open /sys/class/gpio/export\n");
        raise(SIGTERM);
    }
    fprintf(fp, "%d", XSHUTPIN);
    fclose(fp);

    // Give the udev rules a chance to make the GPIO available
    sleep(1);

    // Set as output
    sprintf(buf, "/sys/class/gpio/gpio%d/direction", XSHUTPIN);
    fp = fopen(buf, "w");
    if (fp == NULL)
    {
        print("Failed to open %s\n", buf);
        raise(SIGTERM);
    }
    fprintf(fp, "out");
    fclose(fp);

    // Set GPIO4 high
    sprintf(buf, "/sys/class/gpio/gpio%d/value", XSHUTPIN);
    fp = fopen(buf, "w");
    if (fp == NULL)
    {
        print("Failed to open %s\n", buf);
        raise(SIGTERM);
    }
    fprintf(fp, "1");
    fclose(fp);

    // Delay for a bit
    usleep(10000); // 10 millisecond

    // Initialize the i2c bus
    print("Initializing I2C bus...\n");
    Dev->i2c_slave_address = 0x29;
    Dev->fd = VL53LX_i2c_init("/dev/i2c-1", Dev->i2c_slave_address); // choose between i2c-0 and i2c-1; On the raspberry pi zero, i2c-1 are pins 2 and 3
    if (Dev->fd < 0)
    {
        print("Failed to init 4\n");
        raise(SIGTERM);
    }

    status = VL53LX_WaitDeviceBooted(Dev);
    check_status(status);

    status = VL53LX_DataInit(Dev);
    check_status(status);

    // Check if address is default
    if (address == 0x29)
    {
        print("Using default I2C address 0x29\n");
    }
    else
    {
        print("Switching to using I2C address 0x%02X\n", address);
        // set new i2c address for VL53L3CX
        // print address
        status = VL53LX_SetDeviceAddress(Dev, address);
        check_status(status);
        Dev->i2c_slave_address = address;
    }

    pDev = VL53LXDevStructGetLLDriverHandle(Dev);
    print("Device Info:\n");
    print("\t Product Type : 0x%02X\n", pDev->nvm_copy_data.identification__module_type);
    print("\t Model ID : 0x%02X\n", pDev->nvm_copy_data.identification__model_id);

    if ((pDev->nvm_copy_data.identification__module_type == 0xAA) &&
        (pDev->nvm_copy_data.identification__model_id == 0xEA))
    {
        print("\t Model Name : VL53L3CX\n");
    }
    else
    {
        print("WARNING: Unknown model ID!\n");
        raise(SIGTERM);
    }

    print("\n");

    // Set distance mode if not default
    if (distance_mode != VL53LX_DISTANCEMODE_MEDIUM)
    {
        print("Setting distance mode to %s\n", distance_mode == VL53LX_DISTANCEMODE_SHORT ? "SHORT" : "LONG");
        status = VL53LX_SetDistanceMode(Dev, distance_mode);
        check_status(status);
    }
    // Set timing budget if not default
    if (timing_budget != 33)
    {
        print("Setting timing budget to %d ms\n", timing_budget);
        status = VL53LX_SetMeasurementTimingBudgetMicroSeconds(Dev, timing_budget * 1000);
        check_status(status);
    }

    status = VL53LX_StartMeasurement(Dev);
    check_status(status);

    ranging_loop();
}

// CTRL-C handler
void signal_handler(int signal)
{

    // Print if not in compact mode

    print("\n\rExiting...\n\r");

    // Turn off the sensor using GPIO4
    char buf[100];
    sprintf(buf, "/sys/class/gpio/gpio%d/value", XSHUTPIN);
    FILE *fp = fopen(buf, "w");
    if (fp == NULL)
    {
        print("Failed to open %s\n", buf);
        return;
    }
    fprintf(fp, "0");
    fclose(fp);

    // Disable GPIO4 using sysfs
    fp = fopen("/sys/class/gpio/unexport", "w");
    if (fp == NULL)
    {
        print("Failed to open /sys/class/gpio/unexport\n");
        // return;
    }
    fprintf(fp, "%d", XSHUTPIN);
    fclose(fp);

    exit(signal);
}

// Ranging loop
void ranging_loop(void)
{
    // Socket to talk to clients
    void *context = zmq_ctx_new();
    void *publisher = zmq_socket(context, ZMQ_PUB);
    // create ip string with tcp port
    char ip_string[20];
    sprintf(ip_string, "tcp://*:%d", tcp_port);
    int rc = zmq_bind(publisher, ip_string);
    assert(rc == 0);

    VL53LX_MultiRangingData_t MultiRangingData;
    VL53LX_MultiRangingData_t *pMultiRangingData = &MultiRangingData;
    VL53LX_AdditionalData_t AdditionalData;
    VL53LX_AdditionalData_t *pAdditionalData = &AdditionalData;

    uint8_t NewDataReady = 0;
    int no_of_object_found = 0;
    int j;
    int is_A;
    char tmp_data1[5], tmp_data2[512], data[3000];
    char histogram_data_buffer[500];
    char bin_buffer[5];

    print("\nRanging started...\n\n");

    do
    { // polling mode

        status = VL53LX_GetMeasurementDataReady(Dev, &NewDataReady);
        check_status(status);

        usleep(poll_period * 1000); // Polling period

        if ((!status) && (NewDataReady != 0))
        {

            status = VL53LX_GetMultiRangingData(Dev, pMultiRangingData);
            check_status(status);

            /*
            From: https://community.st.com/s/question/0D53W00000etcEZ/understanding-vl53l3cx-histogram-data
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

            From: https://community.st.com/s/question/0D53W00001Gl6B2SAJ/can-someone-please-post-sample-code-on-how-to-get-histogram-data-from-vl53l3cx-i-have-the-dev-board-nucleo-f401-re-and-the-both-the-sensor-and-the-eval-kit

            But it's not quite that easy. There are 2 ranges and they toggle back an forth. One range consists of 4 bins of ambient light and 20 data bins.
            The alternating range consists of 24 range bins.
            Range on a flat wall print out the 24 bins, gathering the data into a spreadsheet.
            You will soon figure it out.
            (The two range timing have to do with a search for 'radar aliasing' effects. google it.)
            And each bin is about 20cm worth of distance.
            */

            no_of_object_found = pMultiRangingData->NumberOfObjectsFound;

            // Process if object is found
            if (no_of_object_found > 0)
            {

                // Check if even
                is_A = (pMultiRangingData->StreamCount % 2 == 0);
                //
                if ((hist_mode == HIST_A && is_A) || (hist_mode == HIST_B && !is_A) || hist_mode == HIST_BOTH)
                {
                    if (!compact_flag)
                    {
                        printf("Count:     %d,\n", pMultiRangingData->StreamCount);
                        printf("# Objs:    %1d\n", no_of_object_found);
                    }

                    sprintf(tmp_data1, "%d ", pMultiRangingData->StreamCount);
                    strcat(data, tmp_data1);
                    memset(tmp_data1, 0, sizeof(tmp_data1));

                    // if (hist_flag)
                    // {

                    VL53LX_GetAdditionalData(Dev, pAdditionalData);

                    // Convert the histogram data to a comma-separated string
                    if ((hist_mode == HIST_A && is_A) || (hist_mode == HIST_BOTH && is_A))
                    {
                        for (j = 5; j < VL53LX_HISTOGRAM_BUFFER_SIZE; j++)
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
                    }
                    else if ((hist_mode == HIST_B && !is_A) || (hist_mode == HIST_BOTH && !is_A))
                    {
                        for (j = 1; j < VL53LX_HISTOGRAM_BUFFER_SIZE; j++)
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
                    }

                    strcat(data, histogram_data_buffer);

                    if (!compact_flag)
                    {
                        printf("Histogram: %s\n", histogram_data_buffer);
                    }

                    // }
                    for (j = 0; j < no_of_object_found; j++)
                    {

                        if (!compact_flag)
                        {
                            printf("Status=%d, Min Dist=%d mm, Dist=%d mm, Max dist=%d mm, Sigma=%2.2f mm, Signal Rate=%2.2f Mcps, Ambient Rate=%2.2f Mcps\n",
                                   pMultiRangingData->RangeData[j].RangeStatus,
                                   pMultiRangingData->RangeData[j].RangeMinMilliMeter,
                                   pMultiRangingData->RangeData[j].RangeMilliMeter,
                                   pMultiRangingData->RangeData[j].RangeMaxMilliMeter,
                                   pMultiRangingData->RangeData[j].SigmaMilliMeter / 65536.0,
                                   pMultiRangingData->RangeData[j].SignalRateRtnMegaCps / 65536.0,
                                   pMultiRangingData->RangeData[j].AmbientRateRtnMegaCps / 65536.0);
                        }

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
                    }

                    rc = zmq_send(publisher, data, strlen(data), 0);

                    if (compact_flag)
                    {
                        printf("%s\n", data);
                    }
                    else
                    {
                        printf("\n");
                    }

                    // clear buffers
                    // if (hist_flag)
                    // {
                    memset(histogram_data_buffer, 0, sizeof(histogram_data_buffer));
                    memset(bin_buffer, 0, sizeof(bin_buffer));
                    // }
                    memset(data, 0, sizeof(data));
                }
            }
        }

        status = VL53LX_ClearInterruptAndStartMeasurement(Dev);
        check_status(status);

    } while (1);

    zmq_close(publisher);
    zmq_ctx_destroy(context);
}
