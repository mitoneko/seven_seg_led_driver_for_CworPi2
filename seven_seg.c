/****************************************************************
 *  CrowPi2用　7セグメントLED　ドライバ
 ****************************************************************/

#define DEBUG 1
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/i2c.h>

#define DRIVER_NAME "sevenseg"

// SevenSegドライバーの固有情報
struct sevenseg_device_info {
    struct i2c_client *client ;
    u8 brightness; // 1-16 大きいほど明るい
    u8 blink_mode; // 0-3  0:常時点灯 1-3:大きいほど早い(0.5Hz,1Hz,2Hz)
    bool led_on;
};
    


static const unsigned int MINOR_BASE = 0; // udev minor番号の始まり
static const unsigned int MINOR_NUM = 1;  // udev minorの個数

// HT16K33 OSC ON・OFF
s32 ht16_osc_ctl(struct sevenseg_device_info *dev_info, bool osc) {
    u8 send_data;

    // OSC ON・OFFコマンド　ON:0x21 OFF:0x20
    send_data = 0x20 | (int)osc;
    pr_devel("%s: send_data=[%02X]\n", __func__, send_data); 
    return i2c_smbus_write_byte(dev_info->client, send_data);
}

// HT16K33 LED ON・OFF & Blink Set
// 引数:
//  led: 真でon
//  blink: 0-3 0->常時点灯　1-3->大きいほど早い(0.5hz, 1hz, 2hz)
s32 ht16_led_ctl(struct sevenseg_device_info *dev_info, bool led, u8 blink) {
    u8 send_data;
    u8 blink_cmd;

    // LEDコントロール　 ベース:0x80 
    switch (blink) {
        case 1: // 0.5Hz
            blink_cmd = 0x06;
            break;
        case 2: // 1Hz
            blink_cmd = 0x04;
            break;
        case 3: // 2Hz
            blink_cmd = 0x02;
            break;
        case 0: // 常時点灯
        default:
            blink = 0;
            blink_cmd=0;
    };

    send_data = 0x80 | (int)led | blink_cmd;
    dev_info->led_on = led;
    dev_info->blink_mode = blink;
    pr_devel("%s: send_data=[%02X]\n", __func__, send_data); 
    return i2c_smbus_write_byte(dev_info->client, send_data);
}

// HT16K33 明るさの設定
// 　引数:brightness 1-16 数字が大きいほど明るい
s32 ht16_dimmer_ctl(struct sevenseg_device_info *dev_info, u8 brightness) {
    u8 send_data;

    // Dimmerコントロール
    // ベース:0xE0
    // 下位4ビットで明るさ
    if (brightness>16) brightness=16;
    dev_info->brightness = brightness;
    send_data = 0xE0 | (brightness-1);
    return i2c_smbus_write_byte(dev_info->client, send_data);
}
    
// 7SEG LED ドライバーチップ　HT16K33 初期化
u32 sevenseg_chip_initialize(struct sevenseg_device_info *dev_info) {
    // OSC:0n LED:On 点滅:無し　明るさ:10 表示:空白
    u32 result;
    u8 buf[7]={0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x08};

    result = ht16_osc_ctl(dev_info, true);
    if (result < 0) return result;
    result = ht16_led_ctl(dev_info, true, 0);
    if (result < 0) return result;
    result = ht16_dimmer_ctl(dev_info, 10);
    if (result < 0) return result;
    result = i2c_smbus_write_i2c_block_data(dev_info->client, 0x00, 7, buf);
    if (result != 0) pr_alert("%s: HT16K33 initialize fail[%d]\n", __func__, result);
    return result;
}

// 7SEG LED ドライバーチップ　HT16K33 オフ(表示・OSCをともにカット)
u32 sevenseg_chip_off(struct sevenseg_device_info *dev_info) {
    u32 result;

    result = ht16_led_ctl(dev_info, false, 0);
    if (result < 0) return result;
    return ht16_osc_ctl(dev_info, false);
}

// sysFS関係

// sysFS LEDのブリンク制御へのアクセス関数
static ssize_t read_sysfs_blink(struct device *dev, struct device_attribute *sttr, char *buf) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);

    return snprintf(buf, PAGE_SIZE, "%d\n", dev_info->blink_mode);
}

static ssize_t write_sysfs_blink(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);
    int input_value;
    int result;

    if (count > 3) return -EINVAL;
    result = kstrtoint(buf, 10, &input_value);
    if (result != 0) return -EINVAL;
    if (input_value < 0 || input_value > 3) return -EINVAL;
    result = ht16_led_ctl(dev_info, dev_info->led_on, input_value);
    if (result != 0) return -EIO;
    return count;
}

struct device_attribute attr_blink = {
    .attr = {
        .name = "blink",
        .mode = S_IRUGO | S_IWUGO,
    },
    .show = read_sysfs_blink,
    .store = write_sysfs_blink,
};


// sysFS LEDのON・OFFへのアクセス関数
static ssize_t read_sysfs_ledon(struct device*dev, struct device_attribute *attr, char *buf) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);

    return snprintf(buf, PAGE_SIZE, "%d\n", (int)dev_info->led_on);
}

static ssize_t write_sysfs_ledon(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);
    int input_value;
    int result;

    if (count > 3) return -EINVAL;
    result = kstrtoint(buf, 10, &input_value);
    if (result != 0) return -EINVAL;
    if (input_value!=0 && input_value!=1) return -EINVAL;
    result = ht16_led_ctl(dev_info, (bool)input_value, dev_info->blink_mode);
    if (result != 0) return -EIO;
    return count;
}

struct device_attribute attr_ledon = {
    .attr = {
        .name = "ledon",
        .mode = S_IRUGO | S_IWUGO,
    },
    .show = read_sysfs_ledon,
    .store = write_sysfs_ledon,
};

// sysFS brightnessへのアクセス関数
static ssize_t read_sysfs_brightness(struct device *dev, struct device_attribute *attr, char *buf) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);
    
    return snprintf(buf, PAGE_SIZE, "%d\n", dev_info->brightness);
}

static ssize_t write_sysfs_brightness(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);
    int brightness;
    int result;

    if (count > 3) return -EINVAL;
    result = kstrtoint(buf, 10, &brightness);
    if (result != 0) return -EINVAL;
    if (brightness < 1) brightness = 1;
    if (brightness > 16) brightness = 16;

    result = ht16_dimmer_ctl(dev_info, brightness);
    if (result != 0) return -EIO;
    dev_info->brightness = brightness;
    return count;
}

struct device_attribute attr_brightness = {
    .attr = {
        .name = "brightness",
        .mode = S_IRUGO | S_IWUGO,
    },
    .show = read_sysfs_brightness,
    .store = write_sysfs_brightness,
};

// sysFS 関数たちのシステムへの登録と解除
// 設定用特殊ファイルは、 /sys/bus/i2c/drivers/SevenSeg/1-0070 の下に出来る。
// 本物は、/sys/devices/platform/soc/fe804000.i2c/i2c-1/1-0070である。
static int make_sysfs(struct sevenseg_device_info *dev_info) {
    int result;

    result = device_create_file(&dev_info->client->dev, &attr_brightness);
    if (result != 0) {
        pr_err("sevenseg(%s): fail to create sysfs for brightness.[%d]\n", __func__, result);
        goto err;
    }
    result = device_create_file(&dev_info->client->dev, &attr_ledon);
    if (result != 0) {
        pr_err("sevenseg(%s): fail to create sysfs for ledon.[%d]\n", __func__, result);
        goto err_ledon;
    }
    result = device_create_file(&dev_info->client->dev, &attr_blink);
    if (result != 0) {
        pr_err("sevenseg(%s): fail to create sysfs for blink.[%d]\n", __func__, result);
        goto err_blink;
    }

    return 0;
    
err_blink:
    device_remove_file(&dev_info->client->dev, &attr_ledon);
err_ledon:
    device_remove_file(&dev_info->client->dev, &attr_brightness);
err:
    return result;
}

static void remove_sysfs(struct sevenseg_device_info *dev_info) {
    device_remove_file(&dev_info->client->dev, &attr_brightness);
    device_remove_file(&dev_info->client->dev, &attr_ledon);
    device_remove_file(&dev_info->client->dev, &attr_blink);
}

// ドライバーの登録　及び　削除
static int sevenseg_probe(struct i2c_client *client) {
    u32 result;
    struct sevenseg_device_info *dev_info;

    dev_info = (struct sevenseg_device_info*)devm_kzalloc(&client->dev, sizeof(struct sevenseg_device_info), GFP_KERNEL);
    if (!dev_info) {
        pr_alert("%s: Fail allocate memory for device_info\n", __func__);
        result = -ENOMEM;
        goto err;
    }
    i2c_set_clientdata(client, dev_info);
    dev_info->client = client;
    
    // LEDチップの初期化
    result = sevenseg_chip_initialize(dev_info);
    if (result != 0) goto err;

    // sysFS 特殊ファイルの生成
    result = make_sysfs(dev_info);
    if (result != 0) goto err;
        
    
    pr_info("%s: Seven Segment LED driver installed. i2c_addr=[0x%02X]\n", __func__, client->addr);
    return result;

err:
    return result;
}

static int sevenseg_remove(struct i2c_client *client) {
    struct sevenseg_device_info *dev_info = (struct sevenseg_device_info *)i2c_get_clientdata(client);

    remove_sysfs(dev_info);
    sevenseg_chip_off(dev_info);

    
    pr_info("%s: Seven Segment LED driver removed.", __func__);
    return 0;

}

static struct i2c_device_id i2c_seven_seg_ids[] = {
    {"SevenSegLED", 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, i2c_seven_seg_ids);

static struct of_device_id of_seven_seg_ids[] = {
    {.compatible = "CrowPi2,SevenSegmentLed",},
    {},
};
MODULE_DEVICE_TABLE(of, of_seven_seg_ids);

static struct i2c_driver seven_seg_driver = { 
    .probe_new = sevenseg_probe,
    .remove = sevenseg_remove,
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_seven_seg_ids,
    },
};

module_i2c_driver(seven_seg_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This is 7 segment led driver for CrowPi2.");
MODULE_AUTHOR("mito");
MODULE_VERSION("0.0.0");
