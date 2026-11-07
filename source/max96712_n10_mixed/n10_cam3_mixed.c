/*
 * max96712.c - max96712 IO Expander driver
 *
 * Copyright (c) 2016-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

//  #define DEBUG

#ifndef __SL_DESERIALIZER__
#define __SL_DESERIALIZER__
#define __MAX96712__
#else
#error Error, a deserializer is already compiled. Fix the defconfig and use only one deserializer.
#endif

#include <linux/seq_file.h>
#include <media/camera_common.h>
#include <media/tegracam_core.h>
#include <linux/module.h>
#include "sl_max96712_mode_tbls.h"
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/types.h>
#include <linux/platform_device.h>
/* N10 mixed CAM-3: retain the vendor I2C driver and its SHW5G private data.
 * This explicitly loaded platform client configures only ZED link 3/pipes 4,5.
 * Run the vendor V6/V7 initialization first, with all capture clients stopped. */

#define STRINGIFY0(s)                                                       
#define STRINGIFY(s) STRINGIFY0(s)                                              
#define DRV_STR_VERSION STRINGIFY(DESER_DRIVER_VERSION_MAJOR)"."STRINGIFY(DESER_DRIVER_VERSION_MINOR)"."STRINGIFY(DESER_DRIVER_VERSION_PATCH)"" 

//int write_reg_Ser(int slaveAddr, int channel, u16 addr, u8 val);
int set_bitrate_Dser(int channel, u32 i2c_bus, u8 val);
u32 fps_set_Dser(int channel, s64 val);
int dser_get_gmsl_port(int channel, int zedx_id);
int dser_enable_gmsl_link(int channel, int zedx_id);
int dser_open_all_gmsl_link(int channel);
int isSecondCamFromI2C(int channel, int zedx_id);
int dser_read_video_lock(int channel, int zedx_id);
int dser_read_link_lock(int channel, int zedx_id);

struct sensor
{
    struct list_head list;
    int detection_id; /* ID of the camera when connected to the deser */
    s8 pipes[N_SER_PIPES]; /* table position gives ser pipe, [ X Y Z U ],
                            * table content gives deser pipe [ 0 1 -1 -1] */
    const char *camera;
    s8 cam_dts_id; /* ID of the camera when detected in dts */
    int model; /* Camera model ID */
    u32 vc_id; /* CSI Virtual channel ID */
    u32 n_lanes; /* Jetson number of CSI lanes */
    u32 serial; /* Jetson CSI connection */
    u32 i2c_bus; /* i2c bus of the device from dts */
    int gmsl_link; /* GMSL link connected to the device */
    u32 cam_addr;
    u32 ser_addr;
    u32 zedx_id;
    int phy_index;
    int i2c_cc;
    bool is_second_cam_from_i2c;
    int dsr_pipe;
    int phy_rate;
};

/**
 * struct sl_max9295 - ZED-X private data.
 * @i2c_client: struct i2c_client * - i2c adapter of the sl_max9295.
 * @id: struct i2c_device_id * - i2c adapter id.
 * ZED-X private data for this driver. One structure is initialized by camera sensor,
 * so two per ZED-X.
 */
typedef struct serializer_devices{
	int zedx_id;
	int camera_model;
    u32 ser_addr;
}serializer_devices;


/**
 * struct max96712 - Deserializer device structure
 * @i2c_client: I2C client structure for communication with the device
 * @regmap: Register map for the device
 * @channel: Channel ID from device tree
 * @reset_gpio: GPIO number for device reset pin
 * @pwr_gpio: GPIO number for device power pin
 * @pwdn_gpio: GPIO number for device power down pin
 * @port_to_i2c: Mapping of GMSL ports to I2C buses (0,1, and 2)
 *
 * Deserializer device structure containing device-specific information.
 */
struct max96712
{
    bool intialized;
    struct device *owner;
    /* Reference to the vendor's shared active-stream count; no probe-time lease. */
    struct i2c_client *shw_client;
    struct module *shw_owner;
    int *output_users;
    unsigned int zed_output_users;
    struct mutex output_lock;
    struct device_node *config_node;
    struct i2c_client *i2c_client;
    struct regmap *regmap;
    struct list_head sensor_list;
    u32 channel; // channel id from dts
    u32 n_lanes;
    int reset_gpio;
    int pwr_gpio;
    int pwdn_gpio;
    s8 port_to_i2c[N_GMSL_PORTS];
    u8 avail_pipe;
    u8 n_cam;
    struct serializer_devices ser_devices[N_MAX_TOTAL_SER];
    struct sensor detected_sensors[2*N_GMSL_PORTS];
	int mfp_trig_in; // Mfp used as trigger input (default MFP10) 
    int mfp_trig_info; // Mfp used for HW sync mode control (-1 if not used)
};

/**
 * global_priv - Array of pointers to deserializer device structures
 *
 * Array of pointers to deserializer device structures representing connected devices.
 */
struct max96712 *global_priv[4];
static int sync_mode = 0;
module_param(sync_mode, int, 0);

/**
 * write_reg_Dser - Write value to register on MAX96712 deserializer device
 * @channel: Channel ID of the device
 * @addr: Address of the register to write to
 * @val: Value to write to the register
 *
 * This function writes a value to a register on a MAX deserializer device.
 * If the device is connected and the write operation is successful, it returns 0.
 * Otherwise, it returns -1 and logs an error message.
 */
static int write_reg_Dser(int channel, u16 addr, u8 val)
{
    struct i2c_client *i2c_client = NULL;
    int err;

    if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
        return -1;

    i2c_client = global_priv[channel]->i2c_client;

    err = regmap_write(global_priv[channel]->regmap, addr, val);

    if (err)
    {
        dev_err(&i2c_client->dev, "%s: addr = 0x%x, val = 0x%x\n",
                __func__, addr, val);
        return -1;
    }
    return 0;
}

#if 0
/**
 * read_reg_Dser - Read value from register on MAX deserializer device
 * @channel: Channel ID of the device
 * @addr: Address of the register to read from
 * @val: Pointer to store the read value
 *
 * This function reads a value from a register on a MAX deserializer device
 * and stores it in the variable pointed to by @val. If the device is connected
 * and the read operation is successful, it returns 0. Otherwise, it returns -1
 * and logs an error message.
 */
static int read_reg_Dser(int channel, u16 addr, unsigned int *val)
{
    struct i2c_client *i2c_client = NULL;
    int err;

    if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
        return -1;

    i2c_client = global_priv[channel]->i2c_client;

    err = regmap_read(global_priv[channel]->regmap, addr, val);
    if (err)
    {
        dev_err(&i2c_client->dev, "%s: addr = 0x%x, val = 0x%x\n",
                __func__, addr, *val);
        return -1;
    }
    return 0;
}

int write_reg_Ser(int slaveAddr, int channel, u16 addr, u8 val)
{
    struct i2c_client *i2c_client = NULL;
    int bak = 0;
    int err;
    /* unsigned int ival = 0; */

    if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
        return -1;
    i2c_client = global_priv[channel]->i2c_client;
    bak = i2c_client->addr;

    i2c_client->addr = slaveAddr;
    dev_err(&i2c_client->dev, "%s: Slave addr = %x\n",
                __func__, slaveAddr);
    err = regmap_write(global_priv[channel]->regmap, addr, val);

    i2c_client->addr = bak;
    if (err)
    {
        dev_err(&i2c_client->dev, "%s: addr = 0x%x, val = 0x%x\n",
                __func__, addr, val);
        return -1;
    }
    return 0;
}
EXPORT_SYMBOL(write_reg_Ser);
#endif

static bool isCameraMono (struct sensor *sp)
{
    if(!strcmp(sp->camera,"zedonehdr") || !strcmp(sp->camera,"zedone4k") || !strcmp(sp->camera,"zedonegs"))
        return true; 
    else
        return false;
}

static inline int update_ISX_nor_flash(struct max96712 *priv, int addr, int force_update){
	int j = 0;
	int err = 0;
	struct i2c_client *client = priv->i2c_client;
    unsigned int register_val = 0;

    struct i2c_fingerprint update_flash_table[] = {
			{0x1a, 0x8A54, addr}, // Image sensor @ x10 unlock reg
			{0x1a, 0xFFFF, 0xF4}, // Image sensor @ x10 unlock reg
			{0x1a, 0xFFFF, 0xF7}, // Image sensor @ x10 unlock reg
			{0x1a, 0x8000, 0x04}, // Image sensor @ x10 unlock reg
			{0x1a, 0x8001, 0x19}, // Image sensor @ x10 unlock reg
			{0x1a, 0x8005, 0x5a}, // Image sensor @ x10 unlock reg
            {SLEEP, 	0x00, 	0x00},
			{0x1a, 0xFFFF, 0xF5}, // Image sensor @ x10 unlock reg
			{MAX96712_TABLE_END, 0x00, 0x00},
	};

    // Check if the ISX NOR flash already contains the right I2C address
    if(!force_update){
        err = regmap_read(priv->regmap, 0x8A54, &register_val);
        msleep(12);

        if(register_val == addr && !err){
            dev_dbg(&client->dev, "%s: ISX NOR flash already contains the right I2C address\n", __func__);
            return err;
        }
    }

    // Update the ISX NOR flash with the new I2C address
    dev_dbg(&client->dev, "%s: Update ISX NOR flash with new I2C address 0x%x\n", __func__, addr);
	while (update_flash_table[j].i2c_addr!=MAX96712_TABLE_END){
		client->addr = update_flash_table[j].i2c_addr;

        if(update_flash_table[j].i2c_addr==SLEEP){
            msleep(100);
            j++;
            continue;
        }

		err = regmap_write(priv->regmap, update_flash_table[j].reg_addr, update_flash_table[j].val);
		if(err)
			return err;

		msleep(12);
		j++;
	}

    dev_dbg(&client->dev, "%s: ISX NOR flash updated with new I2C address 0x%x\n", __func__, addr);

	return err;
}

static inline int apply_alternate_mapping(struct max96712 *priv, struct sensor *sp){
	struct i2c_client *client = priv->i2c_client;
	int deser_addr = client->addr;
	struct i2c_fingerprint alt_i2c[60];
	int err = 0;
	u8 j = 0, update_isx_address = 0, force_update = 0;
    u32 ser_addr = sp->ser_addr , sen_1_addr = sp->cam_addr, sen_2_addr = sp->cam_addr;

    if(!isCameraMono(sp))
        sp = list_entry(sp->list.next, struct sensor, list);
    sen_2_addr = sp->cam_addr;

    dev_dbg(&client->dev, "%s: Apply new addr : Serializer -> @0x%x / sen 0 -> @0x%x / sen 1 -> @0x%x \n",
                __func__, ser_addr, sen_1_addr,sen_2_addr);

    /* N10 control aliases are paired with the X/Y routing below. Both must
     * change together: SDK left is @20/VC3, right is @28/VC0. */
    get_fingerprint_alt_table[sp->model](alt_i2c, sizeof(alt_i2c),
        sp->ser_addr, sen_2_addr, sen_1_addr);

    while (alt_i2c[j].i2c_addr!=MAX96712_TABLE_END){
		client->addr = alt_i2c[j].i2c_addr;

        if(alt_i2c[j].i2c_addr==SLEEP){
            msleep(100);
            j++;
            continue;
        }

        // For ISX only: we need to check whether the address needs to be updated to prevent unnecessary writes to the NOR flash
        update_isx_address = alt_i2c[j].reg_addr == 0x8A54 && 
			(sp->model == ZEDXHDR || sp->model == ZEDONEHDR);
        if(update_isx_address){
            err = update_ISX_nor_flash(priv, alt_i2c[j].val, force_update);
            if(err) {
	            client->addr = deser_addr;
                dev_err(&client->dev, "%s: ISX NOR flash update failed\n", __func__);
                return err;
            }

            j++;
            continue;
        }

        err = regmap_write(priv->regmap, alt_i2c[j].reg_addr, alt_i2c[j].val);

		dev_dbg(&client->dev, "%s: I2C device @0x%x, read returns %d\n",
					__func__, client->addr, err);

		dev_dbg(&client->dev, "%s: value written in 0x%x (if any) : 0x%x",
					__func__,alt_i2c[j].reg_addr, alt_i2c[j].val);

		if (err){
			dev_err(&client->dev, "%s: Cannot write 0x%x in 0x%x\n",
					__func__,alt_i2c[j].val, alt_i2c[j].reg_addr);
			client->addr = deser_addr;

			return -1;
		}

		j++;
	}

	client->addr = deser_addr;

	return 0;
}

static inline void zedxep_patch(struct max96712 *priv,
		struct i2c_fingerprint *fingerprint)
{
	struct i2c_client *client = priv->i2c_client;
	int deser_addr = client->addr;
	unsigned int val = 0;

	client->addr = fingerprint[0].i2c_addr;

	regmap_read(priv->regmap, fingerprint[1].reg_addr, &val);

	if (val == fingerprint[1].val)
		regmap_write(priv->regmap, fingerprint[0].reg_addr, 
				fingerprint[0].val);

	client->addr = deser_addr;
}

static inline int model_reset(struct max96712 *priv, u8 model)
{
	struct i2c_fingerprint *fingerprint = reset_table[model];
	struct i2c_client *client = priv->i2c_client;
	int deser_addr=client->addr;
	u8 j=0;
	int err = 0;

	/* If we need to reset the serializer because we changed its address */
	if (fingerprint[j].reg_addr == 0x0000)
	{

		while (j < 2)
		{
			client->addr = fingerprint[j].i2c_addr;

			/* when j=0, the first write might fail
			 * if we just plugged in the sensor:
			 * hence, we just check the last err */
			err = regmap_write(priv->regmap, fingerprint[j].reg_addr  , fingerprint[j].val);

			msleep(6);

			j++;

		}

		/* In case of success, the serializer is reset and
		 * retrieves its original address: we sleep a total of 100ms */
		if ( err==0 )
			msleep(94);

	}

	/* We can parse the reset table for the current sensor */
	while (fingerprint[j].i2c_addr!=MAX96712_TABLE_END)
	{
		client->addr = fingerprint[j].i2c_addr;

        if(fingerprint[j].i2c_addr == SLEEP )
        {
            msleep(100);
            j++;
            continue;
        }

		err = regmap_write(priv->regmap, fingerprint[j].reg_addr  , fingerprint[j].val);

		dev_dbg(&client->dev, "%s: I2C device @0x%x (reg 0x%x), returns %d\n",
				__func__, fingerprint[j].i2c_addr,fingerprint[j].reg_addr, err);

        if(err)
            return err;


		msleep(6);

		j++;
	}

	client->addr = deser_addr;

	return err;

}

static inline int check_model(struct max96712 *priv, u8 i)
{
	struct i2c_client *client = priv->i2c_client;
	struct i2c_fingerprint *fingerprint = fingerprint_table[i];
	unsigned int val = 0;
	int deser_addr=client->addr;
	int err = 0;
	u8 j = 0;

	/* ZED X EP Patch */
	if (i == ZEDX)
	{
		zedxep_patch(priv, fingerprint);

		/* skip the first entry for ZED X fingerprint */
		j = 1;
	}

	/* We reset the alternative i2c_mapping, if any */
    /* Do not check returned error, try to communicate with empty address */
	err = model_reset(priv, i);
    
	/* We check the i2c fingerprint correspondance with this model */
	while (fingerprint[j].i2c_addr!=MAX96712_TABLE_END)
	{

		client->addr = fingerprint[j].i2c_addr;

		msleep(10);

		err = regmap_read(priv->regmap, fingerprint[j].reg_addr  , &val);

		dev_dbg(&client->dev, "%s: I2C device @0x%x (reg 0x%x), read returns %d\n",
				__func__, fingerprint[j].i2c_addr, fingerprint[j].reg_addr, err);

		if (fingerprint[j].val != val || err)
		{
			client->addr = deser_addr;

			return -1;
		}

		j++;
	}

	client->addr = deser_addr;


	return 0;
}

// Reset serializer at 3Gbps and set it to 6Gpbs gmsl speed to match other camera speed
// Necessary for one HDR
static inline int configure_3Gbps_cameras_to_6Gbps(struct max96712 *priv, int link){
	int j = 0;
	int err = 0;
	struct i2c_client *client = priv->i2c_client;
    int deser_addr = client->addr; //init addr
    const int gmsl_6gbps_mode = 0x22;
    int reg_gmsl_ctrl_addr = (link == 0 || link == 1) ? 0x10 : 0x11;
    int gmsl_3gbps_mode = (link == 0 || link == 2) ? 0x21 : 0x12;

	// Set deserializer at 3Gpbs gmsl speed
	client->addr = deser_addr;
	err = regmap_write(priv->regmap, reg_gmsl_ctrl_addr, gmsl_3gbps_mode);
	dev_dbg(&client->dev, "%s: Switching deserializer mode to 3Gbps: %d %x %x %02x\n",
		__func__, err,client->addr, reg_gmsl_ctrl_addr, gmsl_3gbps_mode);
	msleep(150);

	// Reset every possible serializer at 3Gbps
    for (j = 0; j < N_MAX_TOTAL_SER; j++){
        struct i2c_fingerprint reset_table[] = {
            {priv->ser_devices[j].ser_addr, 0x0010, 0x91}, /* Reset every possible serializer */
        };

        if(!priv->ser_devices[j].ser_addr)
            continue;
        
        client->addr = reset_table[0].i2c_addr;
        err = regmap_write(priv->regmap, reset_table[0].reg_addr, reset_table[0].val);
        dev_dbg(&client->dev, "%s: %d %x %x %02x\n",
                __func__, err,client->addr,reset_table[0].reg_addr,reset_table[0].val);
        
        if(err ==0 ){
            msleep(100);
            break;
        }
        msleep(6);
    }

	// Set serializer at 6Gbps gmsl speed
	client->addr = ZED_ONE_SER_DFLT_ADDR;
	err = regmap_write(priv->regmap, MAX9295_GMSL_LINK_RATE_CTRL, MAX9295_GMSL_6GBPS_MODE);
	dev_dbg(&client->dev, "%s: Switching serializer mode to 6Gbps: %d %x %x %02x\n",
		__func__, err,client->addr,MAX9295_GMSL_LINK_RATE_CTRL,MAX9295_GMSL_6GBPS_MODE);

	// Set deserializer at 6 Gbps gmsl speed
	client->addr = deser_addr;
	err = regmap_write(priv->regmap, reg_gmsl_ctrl_addr, gmsl_6gbps_mode);
	dev_dbg(&client->dev, "%s: Switching deserializer mode to 6Gbps: %d %x %x %02x\n",
		__func__, err,client->addr,reg_gmsl_ctrl_addr,gmsl_6gbps_mode);
	msleep(150);

	return err;
}

static inline int sl_max96712_get_camera_model(struct max96712 *priv)
{
	struct i2c_client *client = priv->i2c_client;
	int err;
	u8 i;

	for (i = 0; i<N_CAM_TYPE; i++)
	{
        dev_dbg(&client->dev, "%s: Check if cam model is %s",
				__func__, camera_names[i]);

		err = check_model(priv, i);

		if (err)
			continue;

		dev_info(&client->dev, "%s: %s camera connected to this port",
				__func__, camera_names[i]);
		
		return i;
	}
    
    return -1;
}

static int sl_max96712_i2c_setup(struct max96712 *priv)
{
    int err;
    u8 val_cx, i, val_cc, val_port;
    s8 i2c_cc;
    
    // Default value to disable all GMSL2 port 
    val_port = 0xF0;

    // Default value to disable any control channel crossover 
    val_cx = 0x00;

    // Default control channel for all gmsl port is i2c0
    val_cc = 0xAA;

    for (i=0; i<N_GMSL_PORTS; i++)
    {
        i2c_cc = priv->port_to_i2c[i]; 

        // if nothing is connected to the port, don't activate it
        // and make the CC configuration default
        /* TODO: support i2c_cc = 2 */
        if (i2c_cc<0 || i2c_cc>1)
            i2c_cc = 0;
        else
            val_port = val_port | 1<<i;

        val_cx = val_cx | (i2c_cc<<(4+i));

        val_cc = val_cc & (0xFF ^ (1<<i2c_cc)<<(2*i));

        val_cc = val_cc | ((2>>i2c_cc)<<(2*i));
        
    }
    
    err = regmap_write(priv->regmap, GMSL_LINKS_EN_REG, 0xF0);

    msleep(SLEEP_TIME);

    err = regmap_write(priv->regmap, GMSL_CC_X_OVR_REG, val_cx);

    err = regmap_write(priv->regmap, GMSL_LINKS_CC_REG, val_cc);

    err = regmap_write(priv->regmap, GMSL_LINKS_EN_REG, val_port);

    msleep(SLEEP_TIME);

    dev_dbg(&priv->i2c_client->dev,"%s: set GMSL to i2c [ %d, %d, %d, %d ]",
        __func__, priv->port_to_i2c[0], priv->port_to_i2c[1], priv->port_to_i2c[2], priv->port_to_i2c[3]);

    return 0;
}

static int setup_sensor_pipe(struct max96712 *priv, struct sensor *sp, u8 ser_pipe)
{
    struct i2c_client *i2c_client = priv->i2c_client;
    struct index_reg_8 *table = pipeline_table[sp->model];
	struct index_reg_8 csi_map_reg = table[0];
	struct index_reg_8 csi_data_rate_reg = table[1];
    int i = 2, j = 0;
    int ret = 0;
    int retry = 5;
    u16 addr;
    u8 val, n_map_out = 0, offset = 0;
    s8 dser_pipe = sp->pipes[ser_pipe];
    u16 vc_id = 0;

    /* We start at index 1 since index 0 corresponds to MIPI output mapping */
    /* While we haven't reach the end of the table */
    while (table[i].addr != MAX96712_TABLE_END)
    {

		/* number of register mapping to output on current pipe
		 * only valid after the OUTPU_STREAM entry in the table */
		n_map_out++;

        /* While we haven't tried to write the register 'retry' times */
        for (j = 0; j < retry; j++)
        {
            /* For the output stream, we need to give the vc_id */
            if (table[i].addr == OUTPUT_STREAM)
            {
                vc_id = sp->vc_id << 6;
				/* number of register mapping to output on current pipe */
				n_map_out = 0;
                continue;
            }

            /* Address the right pipe in the in the deserializer */
            addr = table[i].addr + 0x40 * dser_pipe;
            
            /* Append the vc_id if necessary */
            val = (u8)table[i].val | vc_id;

            /* We try to write in the register and retry if necessary */
            ret = regmap_write(priv->regmap, addr, val);

            if (ret)
            {
                dev_warn(&i2c_client->dev, "write_reg_pipe: try %d\n", j);
                msleep(4);
                // if we have retried 'retry' number of time we exit
                if (j == retry-1)
                    return -1;
                //else, we retry
                continue;
            }
        }

		i++;
    }

	/* Set the CSI serial output for each register map */
	addr = csi_map_reg.addr + 0x40 * dser_pipe;

	/* This is the serial port of the jetson % number of MIPI phy of the
	 * deserializer : it works in 4x2 and 2x4 on ZED Link Dual and Quad.
	 * The modulo is there because we the second Deserializer of the Quad
	 * is connected to serial ports 4 to 8 of the Jetson module.*/
	val = 0;

	for (i=0; i<n_map_out; i++)
		val = val | (u8)((offset + (sp->phy_index%4)) << 2*i);

	ret = regmap_write(priv->regmap, addr, val);

	if (ret)
		dev_warn(&i2c_client->dev, "%s: fail write in csi reg of pipe %d\n",
				__func__, dser_pipe);

    sp->dsr_pipe=dser_pipe;
    
    /* Set the data rate for this CSI Link, there is one cam per I2C so should be all right */
	addr = csi_data_rate_reg.addr + 0x03 * sp->phy_index;

    if(sp->phy_rate > 0){
        csi_data_rate_reg.val = sp->phy_rate;
    }

	/* Vendor initializer owns the shared CSI rate (2 Gbps/lane). */
    ret = 0;
    dev_dbg(&i2c_client->dev, "set pipping : opt-csi-port %d / vc-id %d / csi-rate=%x\n", sp->phy_index, sp->vc_id, csi_data_rate_reg.val);

    return 0;
}

static int sl_max96712_pipes_setup(struct max96712 *priv, struct sensor *sp,
                         int model, u8 link)
{
    struct i2c_client *client = priv->i2c_client;
    int err;
    u8 val;
    u16 pipe_reg;
    u8 i;
    u8 cam_pipping = cam_pipes[model];
	s8 cam_id = -1;

    //first camera of one i2c use X Y pipes 
    //second cam use Z U pipes
    //uses i2c_bus as conflicting values because is_second_cam used for IMU and pipping
    //comes from limitation from max9296
    for(i=0; i < priv->n_cam; i++)
    {
        if(sp->i2c_bus == priv->detected_sensors[i].i2c_bus)
        {
            sp->is_second_cam_from_i2c = true;
            cam_pipping = cam_pipping<<2;
        }
    }
            

    dev_dbg(&client->dev, "%s: n_cam = %d ->  cam_pipping = 0x%x\n",
                     __func__, priv->n_cam, cam_pipping);

    /* For each serializer pipe */
    for (i=0; i<N_SER_PIPES; i++)
    {
        if (priv->avail_pipe >= N_DSER_PIPES)
        {
            dev_warn(&client->dev, "%s: no more pipes available\n",
                     __func__);
            return 0;
        }

		if ((sp->cam_dts_id != cam_id) && cam_id >= 0)
		{
            dev_dbg(&client->dev, "%s: no more sensor for this camera\n",
                     __func__);
			//return 0;
			break;
		}

        if (sp->model != model)
        {
            dev_warn(&client->dev, "%s: Montrouge, we got a problem\n",
                     __func__);
            return 0;
        }

		cam_id = sp->cam_dts_id;

        /* if serializer's pipe i is not used (X Y Z U), continue the loop */
        if ( !(1&(cam_pipping>>i)) )
            continue;

        /**
         * pipe x-u gets assign to pipe 0-7 of the deserializer
         * first, we find an available deserializer pipe */
        pipe_reg = GMSL_PIPES_01_REG + (priv->avail_pipe>>1);

        /**
         * GMSL_PIPES_AB_REG structure is 0bGGxxHHyy
         * where A and B denote deserializer pipes A and B,
         * G and H denote gmsl deserializer ports A (0b00) to D (0b11)
         * and x and y denote serializer pipes X (0b0) to U (0b11) */
        /* This coordinator exclusively owns BOTH pipes 4/5. On this N10,
         * reads of local 0x49 while ZED control forwarding is enabled are
         * not reliable (observed 0x0c while isolated read is 0xdc). A RMW
         * can silently route pipe 4 to the wrong link after a cold boot.
         * Program the complete owned register: pipe 4 = link 3/X (0xc),
         * pipe 5 = link 3/Y (0xd). SHW pipes 1/2 are in other registers.
         * Reject layouts outside the explicitly supported mixed topology.
         */
        if (pipe_reg != GMSL_PIPES_45_REG || link != 3 ||
            cam_pipping != 0x03 || i > 1)
            return -EINVAL;
        val = 0xdc;

        err = regmap_write(priv->regmap, pipe_reg, val);

        if (err)
            return -1;
        
        sp->pipes[i] = priv->avail_pipe;
        
        sp->detection_id = priv->n_cam;
        
        /**
         * For the current pipe, we state the CSI
         * packets we want to forward, with the desired
         * virtual channel id.
         * eg: RAW12, Frame Start, Frame End */
        err = setup_sensor_pipe(priv, sp, i);
        if (err) return err;

        priv->avail_pipe++;

        /**
         * get next sensor of this camera, since we filled the list in the right order,
         * we just need to get next element */
        if (list_is_last(&sp->list, &priv->sensor_list)) break;
        sp = list_entry(sp->list.next, struct sensor, list);

    }

	dev_info(&client->dev, "%s: camera pipeline operational\n", __func__);

    return 0;
}

/**
 * links_check_Dser - Check the links connected to a MAX deserializer device
 * @channel: Channel ID of the device
 * @links: Pointer to an array to store the connected links
 *
 * This function checks which links are connected to a MAX deserializer device
 * on the specified @channel and stores the connected link IDs in the array
 * pointed to by @links. If no links are connected or an error occurs, it returns
 * -1. Otherwise, it returns the number of connected links and populates @links
 * with their IDs.
 */
/* During initialization remote control targets can answer local 0x49 reads.
 * Isolate forwarding for a local status read, then restore N10 I2C channel 0.
 * Called only with all capture clients stopped and the client at 0x49. */
static int n10_read_local_init(struct max96712 *priv, unsigned int reg,
                               unsigned int *value)
{
    int err, restore;
    err = regmap_write(priv->regmap, GMSL_LINKS_CC_REG, 0xff);
    if (err) return err;
    usleep_range(1000, 2000);
    err = regmap_read(priv->regmap, reg, value);
    restore = regmap_write(priv->regmap, GMSL_LINKS_CC_REG, 0xaa);
    usleep_range(1000, 2000);
    return err ? err : restore;
}

static int sl_max96712_gmsl_pipeline_setup(struct max96712 *priv)
{
    struct i2c_client *client = priv->i2c_client;
    int deser_addr=client->addr; //init addr
    struct sensor *sp;
    struct list_head *pos;
    unsigned int link = 0;
    int tab_id = MAX96712_LINK_REGS;
    int err = 0;
    int model;
    bool cam_found, config_supported = false;
    int cam_model_count;
    u8 i,j;
    int reg_gmsl_ctrl_addr = 0;
    int gmsl_3gbps_mode = 0;
    int active_gmsl=0;
    priv->n_cam = 0;
    memset(priv->port_to_i2c, -1, sizeof(priv->port_to_i2c));
    priv->port_to_i2c[1] = 0;
    priv->port_to_i2c[2] = 0;

    priv->avail_pipe = 4;

	dev_dbg(&client->dev, "%s: client addr = 0x%x\n",
			__func__, client->addr);

    for (i = 3; i < N_GMSL_PORTS; i++)
    {
        priv->port_to_i2c[i]=-1;

        err = regmap_write(priv->regmap, GMSL_LINKS_EN_REG, 0xF0|(1<<i));

        msleep(SLEEP_TIME);

        if (err)
            return -1;

        err = n10_read_local_init(priv, mode_table[tab_id][i].addr, &link);

        if (err)
            return -1;

        /* Bit mask to get the essential information: is link i connected?*/
        link = (link & 0x08) >> 3;

        // If the link is not detected, we check if it is a 3Gbps GMSL port
        if (!link)
		{
            reg_gmsl_ctrl_addr = (i == 0 || i == 1) ? 0x10 : 0x11;
            gmsl_3gbps_mode = (i == 0 || i == 2) ? 0x21 : 0x12;
            
            // Set deserializer at 3Gpbs gmsl speed 
            err = regmap_write(priv->regmap, reg_gmsl_ctrl_addr , gmsl_3gbps_mode);
            dev_dbg(&client->dev, "%s: %d %x %x %02x\n",
                __func__, err,client->addr,reg_gmsl_ctrl_addr,gmsl_3gbps_mode);
            msleep(150);
            
            // Read the link status again
            err = n10_read_local_init(priv, mode_table[tab_id][i].addr, &link);
            link = (link & 0x08) >> 3;

            // Set deserializer at 6Gbps gmsl speed
            err = regmap_write(priv->regmap, reg_gmsl_ctrl_addr , 0x22);
            msleep(150);
        }

        if(link)
            active_gmsl++;    
    }

    dev_info(&client->dev,"%s: Active GMSL ports : %d",__func__, active_gmsl);

    for (i = 3; i < N_GMSL_PORTS; i++)
    {
        err = regmap_write(priv->regmap, GMSL_LINKS_EN_REG, 0xF0|(1<<i));

        msleep(SLEEP_TIME);

        if (err || verbosity_level)
            dev_dbg(&client->dev, "%s: write addr = 0x%x, val = 0x%x, err %d\n",
                    __func__, GMSL_LINKS_EN_REG, 0xF0|(1<<i), err);

        err = n10_read_local_init(priv, mode_table[tab_id][i].addr, &link);

        if (err)
        {
            dev_dbg(&client->dev, "%s: write addr = 0x%x, val = 0x%x, err %d\n",
                    __func__, mode_table[tab_id][i].addr, link, err);
            return -1;
        }

        /* Bit mask to get the essential information: is link i connected?*/
        link = (link & 0x08) >> 3;

        // If the link is not detected, we check if it is a 3Gbps GMSL port
        if (!link)
		{
            dev_dbg(&client->dev, "%s: No camera connected to 6Gbps GMSL port %d\n",
                    __func__, i);
            
            reg_gmsl_ctrl_addr = (i == 0 || i == 1) ? 0x10 : 0x11;
            gmsl_3gbps_mode = (i == 0 || i == 2) ? 0x21 : 0x12;
            
            // Set deserializer at 3Gpbs gmsl speed 
            err = regmap_write(priv->regmap, reg_gmsl_ctrl_addr , gmsl_3gbps_mode);

            msleep(150);
            
            // Read the link status again
            err = n10_read_local_init(priv, mode_table[tab_id][i].addr, &link);
            link = (link & 0x08) >> 3;

            if (!link)
            {
                dev_info(&client->dev, "%s: No camera connected to GMSL port %d\n",
                        __func__, i);

                // Set deserializer at 6Gbps gmsl speed
                err = regmap_write(priv->regmap, reg_gmsl_ctrl_addr , 0x22);
                msleep(150);

                continue;
            }
        }
        
        dev_warn(&client->dev, "%s: Camera connected to GMSL port %d\n",
				__func__, i);

        for (j = 0; j < N_MAX_TOTAL_SER; j++){
            struct i2c_fingerprint reset_table[] = {
                {priv->ser_devices[j].ser_addr, 0x0010, 0x91}, /* Reset every possible serializer */
            };

            if(!priv->ser_devices[j].ser_addr)
                continue;
            
            client->addr = reset_table[0].i2c_addr;
            err = regmap_write(priv->regmap, reset_table[0].reg_addr, reset_table[0].val);

            if(err == 0 )
            {
                msleep(100);
                break;
            }
            msleep(6);

        }
        client->addr = deser_addr;

        /* Configure 3Gpbs serializer (one hdr) to 6Gbps*/
        configure_3Gbps_cameras_to_6Gbps(priv, i);

        /* read the camera fingerprint and return its ID */
        model = sl_max96712_get_camera_model(priv);
        
        if (model<0)
        {
            dev_warn(&client->dev, "%s: Camera model unknown\n", __func__);
            continue;
        }

        cam_found = false;
        cam_model_count=0;
        /* for each sensor from the dts */
        list_for_each(pos, &priv->sensor_list)
        {
            sp = list_entry(pos, struct sensor, list);
            
            if (sp == NULL)
                return -1;

            /* if not the right model or this model has been assigned already
             * we continue looking for an available camera */
            if (!(sp->model == model))
                continue;

            /* For 2 or less cameras, we keep the same behavior so that each get 
             * its own csi port. Dummies are not taken into account */
            if(active_gmsl <= 2)
            {
                if(sp->detection_id >= 0)
                    continue;

                /* first camera probed will take the first available entry 
                 * second camera probed will take first entry with a different csi port */
                if(priv->n_cam > 0)
                    if(sp->serial == priv->detected_sensors[0].serial)
                        continue;

                if(sp->zedx_id == -1)
                    continue;
            }
            /* More than 2 cameras configuration, associate right DT entry to the right GMSL
             * Allows bandwidth optimization base on the plug-in order 
             * example : 1st ZED X entry in camera-serializers associated to GMSL #0 
             * while 3nd ZED X entry associated to GMSL #2 */
            else
            {
                if(cam_model_count != i)
                {
                    cam_model_count++;
                    config_supported=false;
                    /* If camera Stereo, skip second sensor */
                    if(!isCameraMono(sp))
                        pos = pos->next;
                    continue;
                }

                if(sp->zedx_id == -1)
                {
                    config_supported=false;
                    break;
                }
            }

			/* GMSL port i will be connected to the i2c bus priv->avail_i2c_bus[model] */
			priv->port_to_i2c[i] = sp->i2c_cc;
            sp->gmsl_link = i;
            sp->dsr_pipe = -1;
            
            /* we found the right sensor to initialize a camera */
            cam_found = true;
            config_supported = true;

            break;

        }

        if (!cam_found)
        {
            if(!config_supported)
            {
                dev_err(&client->dev, "%s: Camera plugged in GMSL #%d wrongly placed. Check user guide for camera placement info  \n", __func__, i);
                return -EINVAL;
            }
            else
            {
                dev_warn(&client->dev, "%s: Known camera connected, but entry not found in DTS.\n",
                        __func__);
                dev_warn(&client->dev, "%s: Do you have the right DTS?\n",
                        __func__);
                return -EINVAL;
            }

			continue;
        }

        sp->is_second_cam_from_i2c = false;
        err = sl_max96712_pipes_setup(priv, sp, model, i);
        if (err) return err;

        priv->detected_sensors[priv->n_cam] = *sp;
        priv->n_cam++;

        dev_info(&client->dev, "%s: GMSL #%d : Link Camera %s (id: %d) to port-index %d",__func__,i,sp->camera,sp->zedx_id,sp->serial);

        err = apply_alternate_mapping(priv, sp);
        if (err) return err;
    }

    /* enable build the correct i2c_map */
    err = sl_max96712_i2c_setup(priv);
    if (err) return err;

    err = regmap_write(priv->regmap, GMSL_PIPES_ENABLE,
                       0x36); /* SHW5G pipes 1/2, ZED pipes 4/5 only */

    return err;
}

static int sl_max96712_write_table(struct max96712 *priv,
        const struct index_reg_8 table[])
{
    struct i2c_client *i2c_client = priv->i2c_client;
    int i = 0, j = 0;
    int ret = 0;
    int retry = 5;

    // While we haven't reach the end of the table
    while (table[i].addr != MAX96712_TABLE_END)
    {
        // While we haven't tried to write the register 'retry' times
        for (j = 0; j < retry; j++)
        {
            // We try to write the register. 
            ret = write_reg_Dser(priv->channel, table[i].addr, (u8)table[i].val);
            // if the return value is bad
            if (ret && ((table[i].addr != 0x0000) || (table[i].addr != 0x0013)))
            {
                dev_warn(&i2c_client->dev, "write_reg_Dser: try %d\n", j);
                msleep(4);
                // if we have retried 'retry' number of time we exit
                if (j == retry-1)
                    return -1;
                //else, we retry
                continue;
            }
            // If we write a reset register
            if (0x0013 == table[i].addr || 0x0000 == table[i].addr ||
                    0x0018 == table[i].addr || 0x0006 == table[i].addr)
                msleep(100);
            else if (0x0003 == table[i].addr || 0x0007 == table[i].addr) 
                msleep(30);
            break;
        }
        i++;
    }
    return 0;
}

// configuration for slave/master mode
// sync_mode == 0:
//  > Master mode: Internal Fsync + output Fsync on MFP sync (default=MFP10)
// sync_mode == 1:
//  > Master mode deser 1: Internal Fsync + output Fsync on MFP sync (default=MFP10)
//  > Slave mode deser 2: External Fsync + input Fsync on MFP sync (default=MFP10)
// sync_mode == 2:
//  > Slave mode deser 1: External Fsync + input Fsync on MFP sync (default=MFP10)
//  > Slave mode deser 2: External Fsync + input Fsync on MFP sync (default=MFP10)
static int sl_max96712_configure_sync_mode(struct max96712 *priv){
	int err = 0;
	int mfp_trig_info_addr = 0;
    int hw_rqst_slave_mode = 0; // 0: Master mode, 1: Slave mode

    if(sync_mode < 0 || sync_mode > 2){
        sync_mode = 0; // default to master mode
    }

	// Check if the mfp-trig-info has been configured in the device tree
	// This MFP is used to automatically configure the slave mode without having to set sync_mode when loading the driver
    if(priv->mfp_trig_info != -1){
		// MFP0 is 0x2B0 and each GPIO is offset by 3 register
        int offset_link = (priv->mfp_trig_info > 4) ? (priv->mfp_trig_info > 9) ? (priv->mfp_trig_info > 14) ? 
            MAX96712_GPIOA_ADDR+3 : MAX96712_GPIOA_ADDR+2 : MAX96712_GPIOA_ADDR+1 : MAX96712_GPIOA_ADDR;

        mfp_trig_info_addr = offset_link + (3*priv->mfp_trig_info);
		err = regmap_read(priv->regmap, mfp_trig_info_addr, &hw_rqst_slave_mode);

		if(err < 0){ // Retry reading the register
			msleep(6);
			err = regmap_read(priv->regmap, mfp_trig_info_addr, &hw_rqst_slave_mode);
			if(err < 0){
				dev_err(&priv->i2c_client->dev, "%s: Failed to read mfp trig info state (%d) - Defaulting to master mode \n",
                    __func__, err);
				return 0;
			}
		}
		msleep(6);

		// MFP value is written in bit 3, if MFP is HIGH then the board mode = slave
		hw_rqst_slave_mode = (hw_rqst_slave_mode >> 3) & 0x01;
	}

    // Master mode
    if(sync_mode == 0 && !hw_rqst_slave_mode){
        return err;
    }

    // Checking for board mode in Master/Slave configuration
    // Sync mode should be set to 1 and deser should be the first
    // hw_config should be ==0 not to force slave mode
    if((sync_mode == 1 && priv->channel == 0) && !hw_rqst_slave_mode){ 
            return err;
    }

    // At this stage we should only have slave mode request
    // e.g: trig_info requested slave mode, sync_mode was set to 1 or 2
	if(hw_rqst_slave_mode || sync_mode > 0) {
		struct index_reg_8 slave_mode_table[10] = {0};
		err = get_max96712_slave_mode_table(priv->mfp_trig_in, slave_mode_table, sizeof(slave_mode_table));
		if(err){
			dev_err(&priv->i2c_client->dev, "%s: slave mode table failed to initialize: %d\n",
				__func__, err);
			return err;
		}
		err = sl_max96712_write_table(priv, slave_mode_table);
		dev_info(&priv->i2c_client->dev, "Sync mode configured\n");
	}

	return err;
}

/* Preserve vendor lane routing, SHW5G pipes, GPIO triggers and sensor state. */
static int slow_reset_Dser(int channel)
{
    const struct index_reg_8 *t = max96712_2x4_init_table;
    int i, err;
    if (channel != 0 || !global_priv[channel]) return -EINVAL;
    for (i = 0; t[i].addr != MAX96712_TABLE_END; i++) {
        if (t[i].addr < 0x04a0 || t[i].addr > 0x04bf) continue;
        err = write_reg_Dser(channel, t[i].addr, t[i].val);
        if (err) return err;
    }
    return 0;
}

int dser_get_gmsl_port(int channel, int zedx_id){
    int err = -1;
    struct list_head *pos;
	struct sensor *sp;

    if (channel < 0 || channel > 3 || !global_priv[channel] ||
        global_priv[channel]->intialized == 0)
        return err;

    list_for_each(pos, &global_priv[channel]->sensor_list){
		sp = list_entry(pos, struct sensor, list);
        if (sp == NULL){
			return err;
		}

        if (sp->zedx_id != zedx_id)
			continue;
        
        if(sp->gmsl_link == -1)
        {
            dev_info(&global_priv[channel]->i2c_client->dev,
                "%s: sp->gmsl_link == -1\n",__func__);
            return err;
        }

        return sp->gmsl_link + (channel * N_GMSL_PORTS);
    }

    return -1;
}
EXPORT_SYMBOL(dser_get_gmsl_port);

static int get_video_pipe(int channel, int zedx_id){
    int err = -1;
    struct list_head *pos;
    struct sensor *sp;

    list_for_each(pos, &global_priv[channel]->sensor_list){
        sp = list_entry(pos, struct sensor, list);
        if (sp == NULL){
            return err;
        }

        if (sp->zedx_id != zedx_id)
            continue;

        if(sp->dsr_pipe == -1)
        {
            dev_err(&global_priv[channel]->i2c_client->dev,
                "%s: Invalid video pipe value\n",__func__);
            return err;
        }

        return sp->dsr_pipe;
    }

    return -1;
}

int dser_read_link_lock(int channel, int zedx_id){
    int err = -1;
    int val = 0;
    int gmsl_link = -1;

    if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
        return err;

    gmsl_link = dser_get_gmsl_port(channel, zedx_id);
    gmsl_link = gmsl_link - (channel * N_GMSL_PORTS); // (channel * N_GMSL_PORTS) is added in get gmsl port to take into account board with multiple deser

    if(gmsl_link < 0 || gmsl_link >= N_GMSL_PORTS)
        return err;

    err = regmap_read(global_priv[channel]->regmap, max96712_link_regs[gmsl_link].addr, &val);

    if(err)
        return err;

    val = (val >> 3) & 0x01;

    return val;
}
EXPORT_SYMBOL(dser_read_link_lock);

int dser_read_video_lock(int channel, int zedx_id){
    int err = -1;
    int val = 0;
    int dsr_pipe = -1;

    if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
        return err;

    dsr_pipe = get_video_pipe(channel, zedx_id);

    if(dsr_pipe < 0 || dsr_pipe >= N_DSER_PIPES)
        return err;

    err = regmap_read(global_priv[channel]->regmap, (VIDEO_LOCK_STATUS_REG + 0x20*dsr_pipe), &val);
    if(err)
        return err;

    val = val & 0x01;

    return val;
}
EXPORT_SYMBOL(dser_read_video_lock);

int dser_enable_gmsl_link(int channel, int zedx_id){
	int err = -1;
	struct list_head *pos;
	struct sensor *sp;

    if (channel < 0 || channel > 3 || !global_priv[channel] ||
        global_priv[channel]->intialized == 0)
        return err;
    
    list_for_each(pos, &global_priv[channel]->sensor_list){
		sp = list_entry(pos, struct sensor, list);
		if (sp == NULL){
			return err;
		}
      
        if (sp->zedx_id != zedx_id)
			continue;

        if(sp->gmsl_link == -1)
        {
        dev_info(&global_priv[channel]->i2c_client->dev,
            "%s: sp->gmsl_link == -1\n",__func__);
            return err;
        }

        err = write_reg_Dser(channel, GMSL_LINKS_EN_REG, 
            0xF6 | (1<<sp->gmsl_link));

        if (err){
            msleep(4);
            err = write_reg_Dser(channel, GMSL_LINKS_EN_REG, 
            0xF6 | (1<<sp->gmsl_link));
        }

        if (err)
            return -1;

        dev_dbg(&global_priv[channel]->i2c_client->dev,
            "%s: open GMSL link %d for zedx-id %d\n",
            __func__, sp->gmsl_link, zedx_id);

        return sp->gmsl_link + (channel * N_GMSL_PORTS);
    }

    return -1;
}
EXPORT_SYMBOL(dser_enable_gmsl_link);

int dser_open_all_gmsl_link(int channel){
    int err = -1;

    if (channel < 0 || channel > 3 || !global_priv[channel] ||
        global_priv[channel]->intialized == 0)
        return err;
        
    err = write_reg_Dser(channel, GMSL_LINKS_EN_REG, 
            0xFE);

    if(err){
        msleep(4);
        err = write_reg_Dser(channel, GMSL_LINKS_EN_REG, 
            0xFE);
    }

    return err;
}
EXPORT_SYMBOL(dser_open_all_gmsl_link);

int set_bitrate_Dser(int channel, u32 i2c_bus, u8 val)
{
	struct sensor *sp;
	struct list_head *pos;
	int err = -1;
	u8 reg, i;
	u16 addr;

	if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
		return -1;

	if (val == 10)
	{
		reg = GMSL_10BIT_MODE;
	}

	if (val == 12)
	{
		reg = GMSL_12BIT_MODE;
	}

	list_for_each(pos, &global_priv[channel]->sensor_list)
	{
		sp = list_entry(pos, struct sensor, list);
		if (sp == NULL)
		{
			return -1;
		}

		/* if not the right bus or this model was not detected
		 * we continue looking for an available camera */
		if (!(sp->i2c_bus == i2c_bus) || sp->detection_id < 0)
			continue;

		for (i = 0; i<N_SER_PIPES; i++)
		{
			if (sp->pipes[i]<0) 
				continue;

			addr = GMSL_VID_ST_MAP+0x40*sp->pipes[i];

			err = write_reg_Dser(channel, addr, reg);

			addr = addr+1;

			reg = reg | (sp->vc_id<<6);

			err = write_reg_Dser(channel, addr, reg);

		}
	}

	return err;
}
EXPORT_SYMBOL(set_bitrate_Dser);

int isSecondCamFromI2C(int channel, int zedx_id)
{
    struct sensor *sp;
    u8 i = 0;

    if (channel < 0 || channel > 3 || !global_priv[channel] ||
        global_priv[channel]->intialized == 0)
        return -1;

    for( i=0; i < global_priv[channel]->n_cam; i++)
    {
        sp = &global_priv[channel]->detected_sensors[i];
        if( zedx_id == sp->zedx_id)
        {
            return sp->is_second_cam_from_i2c;
        }
    }
    return -1;
}
EXPORT_SYMBOL(isSecondCamFromI2C);

u32 fps_set_Dser(int channel, s64 val)
{
    int err = -1;
    int tab_id = -1;

    if (channel > 3 || channel < 0 || global_priv[channel] == NULL)
        return -1;

    if (val == 7000000)
    {
        tab_id = MAX96712_7_FPS;
    }

    // 15fps full resolution
    if (val > 14000000 && val < 16000000)
    {
        tab_id = MAX96712_15_FPS;
    }
    
    // 30fps full resolution
    if ((val > 24000000) && (val < 26000000)) 
    {
        tab_id = MAX96712_25_FPS;
    }

    // 30fps full resolution
    if ((val > 29000000) && (val < 31000000)) 
    {
        tab_id = MAX96712_30_FPS;
    }

    // 60fps full resolution
    if ((val > 59000000) && (val < 61000000)) 
    {
        tab_id = MAX96712_60_FPS;
    }

    // 120fps full resolution
    if (val == 120000000)
    {
        tab_id = MAX96712_120_FPS;
    }

    // It is not really an error to ask for a wrong fps value
	if (tab_id == -1)
	{
		dev_dbg(&global_priv[channel]->i2c_client->dev,
				"%s: %lld is not a supported value. [30,60,120]*10^6 are supported.\n",
				__func__, val);
		return 0xEEEE;
	}

    err = sl_max96712_write_table(global_priv[channel], mode_table[tab_id]);

    if (err)
        return 0xFFFF;

    return 0;
}
EXPORT_SYMBOL(fps_set_Dser);


static int sl_max96712_parse_serializer_node(struct max96712 *priv,
		struct device_node *ser_node, s8 cam_id)
{
	int err = 0;
    struct i2c_client *i2c_client = priv->i2c_client;
	struct device_node *cam_node;
	struct device_node *mux_node;
	struct device_node *ports_node, *port_node, *endpoint_node;
    struct sensor *sp;
    int n_sensors;
	const char *str;
    u8 i,j;

	if (!ser_node)
		return -1;

    n_sensors = of_count_phandle_with_args(ser_node, "camera-sensors", NULL);

    for (i = 0; i<n_sensors ; i++)
    {
        
        sp = devm_kzalloc(priv->owner, sizeof(*sp), GFP_KERNEL);

        list_add_tail(&sp->list, &priv->sensor_list);
        
        for (j = 0; j<N_SER_PIPES; j++)
        {
            sp->pipes[j] = -1;
        }

        sp->detection_id = -1;
        sp->gmsl_link = -1;
        sp->phy_rate = -1;

        of_property_read_string(ser_node, "camera_model", &sp->camera);
        for (j=0; j<N_CAM_TYPE; j++)
        {
            if (strcmp(sp->camera, camera_names[j])==0)
                sp->model = j;
        }

        /* Parse zedx-id */
        err = of_property_read_string(ser_node, "zedx-id", &str);
        if(err){
            dev_err(&i2c_client->dev, "%s: 'zedx-id' missing in serializer node %s (if it is a dummy, set zedx_id = -1)",__func__,ser_node->full_name);
            return -EINVAL;
        }

        err = kstrtoint(str,10,&sp->zedx_id);
        if(err)
        {
            dev_err(&i2c_client->dev, "%s: zedx-id conversion to int failed", __func__);
            return -1;
        }
        /* if cam is dummy (declared with id = -1 in DTS) */
        if(sp->zedx_id == -1)
            continue;

        /* Sensor's info */
        cam_node = of_parse_phandle(ser_node, "camera-sensors", i);
        if (cam_node==NULL)
        {  
            dev_warn(&i2c_client->dev, "%s: no cam node in ser node...\n", __func__);
            continue;
        }
        of_property_read_u32(cam_node, "reg", &sp->cam_addr);
        dev_dbg(&i2c_client->dev, "%s: CAM ADDR = %x",__func__, sp->cam_addr);

        of_property_read_u32(ser_node, "reg", &sp->ser_addr);
        dev_dbg(&i2c_client->dev, "%s: SER ADDR = %x",__func__, sp->ser_addr);

        priv->ser_devices[cam_id].ser_addr = sp->ser_addr;

        err = of_property_read_u32(cam_node, "reg", &sp->cam_addr);
        if(err){
            of_node_put(cam_node);
            dev_err(&i2c_client->dev, "%s: 'reg' missing in camera node %s",__func__,cam_node->full_name);
            return -EINVAL;
        }

        /* porting info */
        mux_node = of_get_parent(cam_node);
        if (mux_node==NULL)
        {  
            dev_warn(&i2c_client->dev, "%s: no parent node ?\n", __func__);
            of_node_put(cam_node);
            continue;
        }
        of_property_read_u32(mux_node, "reg", &sp->i2c_bus);
        
        of_node_put(mux_node);
        
        ports_node = of_get_child_by_name(cam_node, "ports");
        
        if (ports_node==NULL)
        {  
            dev_warn(&i2c_client->dev, "%s: no ports node amongst children...\n", __func__);
            of_node_put(cam_node);
            continue;
        }
        port_node = of_get_child_by_name(ports_node, "port");
        if (port_node==NULL)
        {  
            dev_warn(&i2c_client->dev, "%s: no port node amongst children...\n", __func__);
            of_node_put(cam_node);
            of_node_put(ports_node);
            continue;
        }
        endpoint_node = of_get_child_by_name(port_node, "endpoint");
        if (endpoint_node==NULL)
        {  
            dev_warn(&i2c_client->dev, "%s: no endpoint node amongst children...\n", __func__);
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            continue;
        }

        err = of_property_read_u32(endpoint_node, "vc-id", &sp->vc_id);
        if(err)
        {
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            of_node_put(endpoint_node);
            dev_err(&i2c_client->dev,"%s: Sensor %d is missing vc-id entry",__func__,sp->zedx_id);
            return -EINVAL;
        }
        if(sp->vc_id<0 || sp->vc_id>3)
        {
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            of_node_put(endpoint_node);
            dev_err(&i2c_client->dev,"%s: Sensor %d VC-ID out of range [0,1,2,3]",__func__,sp->zedx_id);
            return -EINVAL;
        }

        err = of_property_read_u32(endpoint_node, "bus-width", &sp->n_lanes);
        if(err)
        {
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            of_node_put(endpoint_node);
            dev_err(&i2c_client->dev,"%s: Sensor %d is missing bus-width entry",__func__,sp->zedx_id);
            return -EINVAL;
        }
        if(sp->n_lanes != 2 && sp->n_lanes != 4)
        {
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            of_node_put(endpoint_node);
            dev_err(&i2c_client->dev,"%s: Sensor %d bus-width out of range (2 or 4 lanes only)",__func__,sp->zedx_id);
            return -EINVAL;
        }

        err = of_property_read_u32(endpoint_node, "port-index", &sp->serial);
        if(err)
        {
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            of_node_put(endpoint_node);
            dev_err(&i2c_client->dev,"%s: Sensor %d is missing port-index entry",__func__,sp->zedx_id);
            return -EINVAL;
        }

        // /* Parse optional csi-rate, e.g 1600Mhz should be: opt-csi-rate = <1600> */
        // of_property_read_u32(endpoint_node, "opt-csi-rate", &sp->phy_rate);
        of_property_read_u32(endpoint_node, "opt-csi-rate", &sp->phy_rate);
        if(sp->phy_rate > 0){
            sp->phy_rate /= 100;
            sp->phy_rate = sp->phy_rate | (1 << 5); // disable software override
        }

        err = of_property_read_string(endpoint_node, "opt-csi-port", &str);
        if(err)
        {
            of_node_put(cam_node);
            of_node_put(ports_node);
            of_node_put(port_node);
            of_node_put(endpoint_node);
            dev_err(&i2c_client->dev,"%s: Sensor %d is missing opt-csi-port entry",__func__,sp->zedx_id);
            return -EINVAL;
        }
        if(sp->n_lanes == 2)
        {
            sp->phy_index = str[0] - 'c';
            if(sp->phy_index < 0 || sp->phy_index > 3)
            {
                of_node_put(cam_node);
                of_node_put(ports_node);
                of_node_put(port_node);
                of_node_put(endpoint_node);
                dev_err(&i2c_client->dev, "%s: Sensor %d opt-csi-port out of range ! For 2 lanes configuration : c,d,e,f",__func__,sp->zedx_id);
                return -EINVAL;
            }
        }
        else
        {
            sp->phy_index = str[0] - 'a';
            if(sp->phy_index < 0 || sp->phy_index > 1)
            {
                of_node_put(cam_node);
                of_node_put(ports_node);
                of_node_put(port_node);
                of_node_put(endpoint_node);
                dev_err(&i2c_client->dev, "%s: Sensor %d opt-csi-port out of range ! For 4 lanes configuration : a,b",__func__,sp->zedx_id);
                return -EINVAL;
            }
        }

        of_property_read_u32(endpoint_node, "i2c-cc", &sp->i2c_cc);

        priv->n_lanes = sp->n_lanes;

		dev_dbg(&i2c_client->dev, "%s: associated vc-id = %d\n", __func__, sp->vc_id);
		dev_dbg(&i2c_client->dev, "%s: associated n_lanes = %d\n", __func__, sp->n_lanes);
		dev_dbg(&i2c_client->dev, "%s: associated mipi port = %d\n", __func__, sp->serial);
		dev_dbg(&i2c_client->dev, "%s: associated opt-csi-port = %d\n", __func__, sp->phy_index);
		dev_dbg(&i2c_client->dev, "%s: associated i2c control channel = %d\n", __func__, sp->i2c_cc);
        
        of_node_put(cam_node);
        of_node_put(ports_node);
        of_node_put(port_node);
        of_node_put(endpoint_node);
        
        sp->cam_dts_id = cam_id;

        dev_dbg(&i2c_client->dev, "%s: Parse camera %d (%s) -> SER addr : %d | CAM addr : %d", 
            __func__, sp->cam_dts_id, sp->camera, sp->ser_addr, sp->cam_addr);
        dev_dbg(&i2c_client->dev, "%s: (i2c cc %d -> i2c_bus %d) | (phy %d | vc-id %d) -> serial port %d", 
            __func__, sp->i2c_cc, sp->i2c_bus, sp->phy_index ,sp->vc_id, sp->serial);
        dev_dbg(&i2c_client->dev, "%s: -------------------------------------------------------------", 
            __func__);
    }

	return 0;
}

static int sl_max96712_parse_dt(struct max96712 *priv)
{
	int err = 0;
	struct i2c_client *i2c_client = priv->i2c_client;
	struct device_node *np = priv->config_node;
	struct device_node *ser_np;
	//struct of_phandle_args args;
	const char *str;
    int n_serializers;
	s8 i;

	err = of_property_read_string(np, "channel", &str);
	if (err) {
        dev_err(&i2c_client->dev, "%s: Channel not found --> Requires 'channel' entry in DTS\n", __func__);
        return -EINVAL;
    }
	if (priv->channel < 0 ||  priv->channel > 3 ) {
        dev_err(&i2c_client->dev, "%s: Channel value must be : a,b,c,d \n", __func__);
	    return -EINVAL;
    }

	priv->mfp_trig_info = -1;
	err = of_property_read_u32(np, "mfp-trig-info", &priv->mfp_trig_info);
	if (err || (priv->mfp_trig_info < 0 || priv->mfp_trig_info > MAX96712_NB_MFP)){
		dev_dbg(&i2c_client->dev,
				"%s: 'mfp-trig-info' not found or invalid, assuming no trigger info\n", __func__);
		priv->mfp_trig_info = -1;
	}

    priv->mfp_trig_in = -1;
    err = of_property_read_u32(np, "mfp-trig-in", &priv->mfp_trig_in);
    if (err || priv->mfp_trig_in < 0 || priv->mfp_trig_in > MAX96712_NB_MFP){
        dev_dbg(&i2c_client->dev, 
            "%s: 'mfp-trig-in' not found or invalid, defaulting to MFP10\n", __func__);
        priv->mfp_trig_in = 10;
    }

	global_priv[priv->channel] = priv;

    n_serializers = of_count_phandle_with_args(np, "camera-serializers", NULL);

    dev_dbg(&i2c_client->dev, "%s: Number of declared cameras with this deserializer : %d\n",
     __func__, n_serializers);
    
     if(n_serializers % 4 != 0)
        dev_warn(&i2c_client->dev,"%s: Camera not declared for all GMSL link",__func__);

    if(n_serializers % 4 != 0)
        dev_warn(&i2c_client->dev,"%s: Camera not declared for all GMSL link. If you don't want a camera at a certain port, set a dummy instead",__func__);

    /* retrieve all information for each port */
	for ( i = 0 ; i < n_serializers ; i++ )
	{

		ser_np = of_parse_phandle(np, "camera-serializers" , i);

		if ( ser_np == NULL )
		{
			dev_info(&i2c_client->dev,
					"%s: Issue getting node pointer to serializer %d\n", __func__, i);
            continue;
		}
		err = sl_max96712_parse_serializer_node(priv, ser_np , i);

		if (err)
			dev_info(&i2c_client->dev,
					"%s: Issue parsing serializer node %d\n", __func__, i);
	
		of_node_put(ser_np);

	}

	return 0;
}


static int sl_max96712_parse_gpios(struct max96712 *priv)
{
    struct i2c_client *i2c_client = priv->i2c_client;
    struct device_node *node = i2c_client->dev.of_node;
    int gpio = 0;
	gpio = of_get_named_gpio(node, "reset-gpio", 0);

	if (gpio > 0)
	{
		priv->reset_gpio = gpio;
		gpio_direction_output(priv->reset_gpio, 1);
    }
	else
	{
        dev_dbg(&i2c_client->dev, "%s: No reset GPIO in the dts.\n", __func__);
    }

	gpio = of_get_named_gpio(node, "pwdn-gpio", 0);

	if (gpio > 0)
	{
		priv->pwdn_gpio = gpio;
		gpio_direction_output(priv->pwdn_gpio, 1);
    }
	else
	{
        dev_dbg(&i2c_client->dev, "%s: No pwdn GPIO in the dts.\n", __func__);
    }

	gpio = of_get_named_gpio(node, "pwr-gpio", 0);
	if (gpio > 0)
	{
		priv->pwr_gpio = gpio;
		gpio_direction_output(priv->pwr_gpio, 1);
    }
	else
	{
        dev_dbg(&i2c_client->dev, "%s: No pwr GPIO in the dts.\n", __func__);
    }

    return 0;
	
}

static struct regmap_config sl_max96712_regmap_config = {
    .name = "n10-mixed",
    .reg_bits = 16,
    .val_bits = 8,
    .cache_type = REGCACHE_NONE, //No cache for proper reset
};

static void n10_output_release(struct max96712 *priv)
{
    WARN_ON(priv->zed_output_users);
    priv->output_users = NULL;
    if (priv->shw_owner) module_put(priv->shw_owner);
    if (priv->shw_client) put_device(&priv->shw_client->dev);
    priv->shw_owner = NULL;
    priv->shw_client = NULL;
}

static int n10_output_acquire(struct max96712 *priv)
{
    struct device_node *node;
    struct camera_common_data *data;
    struct tegracam_device *tc;
    /* Borrow the verified SHW driver's shared count, without changing it at
     * probe time. Deskew must start after the receiver's STREAMON setup. */
    node = of_find_node_by_path("/bus@0/i2c@810c680000/imx185_f@20");
    if (!node) return -ENODEV;
    priv->shw_client = of_find_i2c_device_by_node(node);
    of_node_put(node);
    if (!priv->shw_client || !priv->shw_client->dev.driver) return -EPROBE_DEFER;
    data = to_camera_common_data(&priv->shw_client->dev);
    if (!data) return -ENODEV;
    tc = data->tegracam_ctrl_hdl ? data->tegracam_ctrl_hdl->tc_dev : NULL;
    if (!tc || strcmp(tc->name, "shw5g_30fps") || !tc->enable_deskew)
        return -EINVAL;
    if (READ_ONCE(*tc->enable_deskew) != 0) return -EBUSY;
    if (!try_module_get(priv->shw_client->dev.driver->owner)) return -ENODEV;
    priv->shw_owner = priv->shw_client->dev.driver->owner;
    priv->output_users = tc->enable_deskew;
    mutex_init(&priv->output_lock);
    return 0;
}

/* Same CSI gate/count as N10's SHW driver. Serialise application state
 * transitions; image streaming itself remains concurrent. */
int n10_zed_output_start(int channel)
{
    struct max96712 *priv;
    int i, err = 0, users;
    static const struct { u16 reg; u8 value; } deskew[] = {
        {0x0903, 0x37}, {0x0943, 0x37}, {0x0983, 0x37},
        {0x09c3, 0x37}, {0x08a0, 0x84}
    };
    if (channel != 0 || !(priv = global_priv[0]) || !priv->output_users)
        return -ENODEV;
    mutex_lock(&priv->output_lock);
    users = READ_ONCE(*priv->output_users);
    if (users < 0 || users >= 4 || priv->zed_output_users >= 2) { err = -EINVAL; goto done; }
    if (!users) {
        for (i = 0; i < ARRAY_SIZE(deskew); ++i) {
            err = regmap_write(priv->regmap, deskew[i].reg, deskew[i].value);
            if (err) goto done;
            msleep(1);
        }
    }
    WRITE_ONCE(*priv->output_users, users + 1);
    priv->zed_output_users++;
    dev_info(priv->owner, "ZED start: shared output users=%d\n", users + 1);
done:
    mutex_unlock(&priv->output_lock);
    return err;
}
EXPORT_SYMBOL_GPL(n10_zed_output_start);

void n10_zed_output_stop(int channel)
{
    struct max96712 *priv;
    int users;
    if (channel != 0 || !(priv = global_priv[0]) || !priv->output_users) return;
    mutex_lock(&priv->output_lock);
    if (!priv->zed_output_users) goto done;
    users = READ_ONCE(*priv->output_users);
    if (WARN_ON(users < 1)) goto done;
    priv->zed_output_users--;
    WRITE_ONCE(*priv->output_users, users - 1);
    if (users == 1) regmap_write(priv->regmap, 0x08a0, 0x00);
    dev_info(priv->owner, "ZED stop: shared output users=%d\n", users - 1);
done:
    mutex_unlock(&priv->output_lock);
}
EXPORT_SYMBOL_GPL(n10_zed_output_stop);

static int n10_mixed_probe(struct platform_device *pdev)
{
    struct device_node *node;
    struct i2c_client *client;
    struct max96712 *priv;
    unsigned int links;
    int err;
    if (!of_machine_is_compatible("nvidia,tegra264")) return -ENODEV;
    node = of_parse_phandle(pdev->dev.of_node, "nvidia,gmsl-dser-device", 0);
    if (!node) return -EINVAL;
    client = of_find_i2c_device_by_node(node);
    of_node_put(node);
    if (!client) return -EPROBE_DEFER;
    if (client->adapter->nr != 7 || client->addr != 0x49 || !client->dev.driver) {
        put_device(&client->dev); return -ENODEV;
    }
    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) { put_device(&client->dev); return -ENOMEM; }
    priv->owner = &pdev->dev;
    priv->config_node = pdev->dev.of_node;
    priv->i2c_client = client;
    INIT_LIST_HEAD(&priv->sensor_list);
    /* Regmap lifetime follows the coordinator, not the persistent vendor client. */
    priv->regmap = regmap_init_i2c(client, &sl_max96712_regmap_config);
    if (IS_ERR(priv->regmap)) { err = PTR_ERR(priv->regmap); goto fail; }
    err = n10_read_local_init(priv, 6, &links);
    if (err || (links & 6) != 6) { err = -EAGAIN; goto fail; }
    err = sl_max96712_parse_dt(priv);
    if (err) goto fail;
    err = slow_reset_Dser(priv->channel);
    if (err) goto fail;
    err = sl_max96712_gmsl_pipeline_setup(priv);
    if (err || priv->n_cam != 1) { err = err ?: -ENODEV; goto fail; }
    err = n10_output_acquire(priv);
    if (err) goto fail;
    priv->intialized = 1;
    platform_set_drvdata(pdev, priv);
    dev_info(&pdev->dev, "N10 CAM3 mixed: SHW5G V6/V7 preserved, ZED V8 pipes 4/5 ready\n");
    return 0;
fail:
    n10_output_release(priv);
    if (priv->channel < 4 && global_priv[priv->channel] == priv) global_priv[priv->channel] = NULL;
    if (!IS_ERR_OR_NULL(priv->regmap)) regmap_exit(priv->regmap);
    put_device(&client->dev);
    return err;
}

static void n10_mixed_remove(struct platform_device *pdev)
{
    struct max96712 *priv = platform_get_drvdata(pdev);
    n10_output_release(priv);
    global_priv[priv->channel] = NULL;
    regmap_exit(priv->regmap);
    put_device(&priv->i2c_client->dev);
}

/* No MODULE_DEVICE_TABLE alias: loading before vendor initialization is invalid. */
static const struct of_device_id n10_mixed_match[] = {
    { .compatible = "n10,cam3-mixed-zedx" }, { }
};
static struct platform_driver n10_mixed_driver = {
    .probe = n10_mixed_probe,
    .remove_new = n10_mixed_remove,
    .driver = { .name = "n10_cam3_mixed", .of_match_table = n10_mixed_match },
};
module_platform_driver(n10_mixed_driver);
MODULE_DESCRIPTION("N10 CAM3 mixed SHW5G/ZED coordinator; preserves vendor deserializer binding");
MODULE_AUTHOR("Derived from Stereolabs MAX96712 driver");
MODULE_LICENSE("GPL v2");
