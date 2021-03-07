/****************************************************************
 *  CrowPi2用　7セグメントLED　ドライバ
 ****************************************************************/

#define DEBUG 1
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/minmax.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/i2c.h>

#define DRIVER_NAME "sevenseg"

// SevenSegドライバーの固有情報
struct sevenseg_device_info {
    u8 brightness; // 1-16 大きいほど明るい
    u8 blink_mode; // 0-3  0:常時点灯 1-3:大きいほど早い(0.5Hz,1Hz,2Hz)
    bool led_on;
    bool is_char_mode;
    u8 led_vram[16]; // LED表示パターン。実使用は、0,2,4,6のみ。
    u8 cur_pos;    // 0-3 左端よりの桁数
    // kernel内のオブジェクト達
    struct cdev cdev;
    struct class *class;
    int major;
    struct i2c_client *client ;
};
    


static const unsigned int MINOR_BASE = 0; // udev minor番号の始まり
static const unsigned int MINOR_NUM = 1;  // udev minorの個数

/******************************************************************
 * HT16K33チップの制御関係
 *****************************************************************/

// HT16K33 OSC ON・OFF
s32 ht16_osc_ctl(struct sevenseg_device_info *dev_info, bool osc) {
    u8 send_data;

    // OSC ON・OFFコマンド　ON:0x21 OFF:0x20
    send_data = 0x20 | (int)osc;
    pr_devel("%s(%s): send_data=[%02X]\n", THIS_MODULE->name, __func__, send_data); 
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
    pr_devel("%s(%s): send_data=[%02X]\n", THIS_MODULE->name, __func__, send_data); 
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
    
//  HT16K33 VRAMの書き込み。vram_buffの内容を出力。
//  引数: init  これをtrueにすると、16バイト書き込む。false時は、先頭7バイトのみ。
u32 ht16_buffer_write(struct sevenseg_device_info *dev_info, bool init) {
    u32 result;
    int write_count = init ? 16 : 7;
    result = i2c_smbus_write_i2c_block_data(dev_info->client, 
            0x00, write_count, dev_info->led_vram);
    return result;
}
    
// 7SEG LED ドライバーチップ　HT16K33 初期化
u32 sevenseg_chip_initialize(struct sevenseg_device_info *dev_info) {
    // OSC:0n LED:On 点滅:無し　明るさ:10 表示:空白
    u32 result;
    int i;
    
    result = ht16_osc_ctl(dev_info, true);
    if (result < 0) goto error;
    result = ht16_led_ctl(dev_info, true, 0);
    if (result < 0) goto error;
    result = ht16_dimmer_ctl(dev_info, 10);
    if (result < 0) goto error;

    for (i = 0; i < 16; i++) {
        dev_info->led_vram[i] = 0x00;
    }
    result = ht16_buffer_write(dev_info, true);
    if (result < 0) goto error;
    dev_info->cur_pos = 0;
    dev_info->is_char_mode = false;
    return 0;

error:
    pr_alert("%s(%s): HT16K33 initialize fail[%d]\n", THIS_MODULE->name, __func__, result);
    return result;
}

// 7SEG LED ドライバーチップ　HT16K33 オフ(表示・OSCをともにカット)
u32 sevenseg_chip_off(struct sevenseg_device_info *dev_info) {
    u32 result;

    result = ht16_led_ctl(dev_info, false, 0);
    if (result < 0) return result;
    return ht16_osc_ctl(dev_info, false);
}
/********************************************************************
 * udev関係　及び、ファイルアクセス関数
 *******************************************************************/
// ファイルアクセス関数
static int sevenseg_open(struct inode *inode, struct file *file) {
    struct sevenseg_device_info *d_dev = 
        container_of(inode->i_cdev, struct sevenseg_device_info, cdev);
    file->private_data = d_dev;
    return 0;
}

static int sevenseg_close(struct inode *inode, struct file *file) {
    return 0;
}

static ssize_t sevenseg_read(struct file *file, char __user *buf, size_t count, loff_t *offset) {
    return count;
}

static ssize_t sevenseg_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {
    struct sevenseg_device_info *dev_info = file->private_data;
    char k_buff[16];
    int result;
    int cnt;
    int i;

    if (count==0) return 0;
    if (copy_from_user(k_buff, buf, min(count,(size_t)16)) != 0) return -EFAULT;
    
    if (dev_info->is_char_mode) {
        // キャラクターモードの処理。今は未実装
        result = count;
    } else {
        cnt = (count > 4) ? 4 : count;
        for (i = 0; i < cnt; i++) {
            dev_info->led_vram[dev_info->cur_pos*2] = k_buff[i];
            dev_info->cur_pos = (dev_info->cur_pos + 1) % 4;
        }
        result = ht16_buffer_write(dev_info, false);
        if (result == 0) result = count;
    }

    return result;
}

static loff_t sevenseg_lseek(struct file *file, loff_t offset, int whence) {
    return 0;
}

/* ハンドラ　テーブル */
struct file_operations distance_fops = {
    .owner    = THIS_MODULE,
    .open     = sevenseg_open,
    .release  = sevenseg_close,
    .read     = sevenseg_read,
    .write    = sevenseg_write,
    .llseek   = sevenseg_lseek,
};

// キャラクタデバイスの登録と、/dev/sevensegの生成
static int make_udev(struct sevenseg_device_info *dev_info, const char* name) { 
    int result = 0;
    dev_t dev;

    /* メジャー番号取得 */
    result = alloc_chrdev_region(&dev, MINOR_BASE, MINOR_NUM, name);
    if (result != 0) {
        pr_alert("%s(%s): Fail alloc_chrdev_region(%d)\n", THIS_MODULE->name, __func__, result);
        goto err;
    }
    dev_info->major = MAJOR(dev);

    /* カーネルへのキャラクタデバイスドライバ登録 */
    cdev_init(&dev_info->cdev, &distance_fops);
    dev_info->cdev.owner = THIS_MODULE;
    result = cdev_add(&dev_info->cdev, dev, MINOR_NUM);
    if (result != 0) {
        pr_alert("%s(%s): Fail to regist cdev.(%d)\n", THIS_MODULE->name, __func__, result);
        goto err_cdev_add;
    }

    /* カーネルクラス登録 */
    dev_info->class = class_create(THIS_MODULE, name);
    if (IS_ERR(dev_info->class)) {
        result =  -PTR_ERR(dev_info->class);
        pr_alert("%s(%s): Fail regist kernel class.(%d)\n", THIS_MODULE->name, __func__, result);
        goto err_class_create;
    }

    /* /dev/sevenseg の生成 */
    device_create(dev_info->class, NULL, MKDEV(dev_info->major, 0), NULL, name);

    return 0;

err_class_create:
    cdev_del(&dev_info->cdev);
err_cdev_add:
    unregister_chrdev_region(dev, MINOR_NUM);
err:
    return result;
}

// キャラクタデバイス及び/dev/sevensegの登録解除
static void remove_udev(struct sevenseg_device_info *dev_info) {
    dev_t dev = MKDEV(dev_info->major, MINOR_BASE);
    device_destroy(dev_info->class, MKDEV(dev_info->major, 0));
    class_destroy(dev_info->class); /* クラス登録解除 */
    cdev_del(&dev_info->cdev); /* デバイス除去 */
    unregister_chrdev_region(dev, MINOR_NUM); /* メジャー番号除去 */
}


/********************************************************************
* sysFS関係
*********************************************************************/
// 入力数値の文字列を数値に変換する
// 返り値: 正常なら0。負の値はエラー
// 引数:
//  buf: 入力文字列  max: 最大値   min: 最小値　value: 戻り値
static int char_to_int(const char *buf, int *value, int max, int min) {
    int ret_value;
    int result;

    result = kstrtoint(buf, 10, &ret_value);
    if (result != 0) return -EINVAL;
    if (ret_value < min || ret_value > max ) return -EINVAL;
    *value = ret_value;
    return 0;
}

// sysFS 書き込みモードの設定
//  1: キャラクターモード
//  0: バイナリモード
static ssize_t read_sysfs_is_char_mode(struct device *dev, struct device_attribute *attr, char *buf) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);

    return snprintf(buf, PAGE_SIZE, "%d\n", dev_info->is_char_mode);
}

static ssize_t write_sysfs_is_char_mode(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);
    int input_value;
    
    if (count > 3) return -EINVAL;
    if (char_to_int(buf, &input_value, 1, 0)) return -EINVAL;  
    dev_info->is_char_mode = (bool)input_value;
    return count;
}

struct device_attribute attr_is_char_mode = {
    .attr = {
        .name = "is_char_mode",
        .mode = S_IRUGO | S_IWUGO,
    },
    .show = read_sysfs_is_char_mode,
    .store = write_sysfs_is_char_mode,
};

// sysFS LEDのブリンク制御へのアクセス関数
static ssize_t read_sysfs_blink(struct device *dev, struct device_attribute *sttr, char *buf) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);

    return snprintf(buf, PAGE_SIZE, "%d\n", dev_info->blink_mode);
}

static ssize_t write_sysfs_blink(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct sevenseg_device_info *dev_info = dev_get_drvdata(dev);
    int input_value;
    int result;

    if (char_to_int(buf, &input_value, 3, 0)) return -EINVAL;
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

    if (char_to_int(buf, &input_value, 1, 0)) return -EINVAL;
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

    if (char_to_int(buf, &brightness, 16, 1)) return -EINVAL;
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
        pr_err("%s(%s): fail to create sysfs for brightness.[%d]\n", THIS_MODULE->name, __func__, result);
        goto err;
    }
    result = device_create_file(&dev_info->client->dev, &attr_ledon);
    if (result != 0) {
        pr_err("%s(%s): fail to create sysfs for ledon.[%d]\n", THIS_MODULE->name, __func__, result);
        goto err_ledon;
    }
    result = device_create_file(&dev_info->client->dev, &attr_blink);
    if (result != 0) {
        pr_err("%s(%s): fail to create sysfs for blink.[%d]\n", THIS_MODULE->name, __func__, result);
        goto err_blink;
    }
    result = device_create_file(&dev_info->client->dev, &attr_is_char_mode);
    if (result != 0) {
        pr_err("%s(%s): fail to create sysfs for is_char_mode.[%d]\n", THIS_MODULE->name, __func__, result);
        goto err_is_char_mode;
    }

    return 0;
    
err_is_char_mode:
    device_remove_file(&dev_info->client->dev, &attr_blink);
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
    device_remove_file(&dev_info->client->dev, &attr_is_char_mode);
}

/***********************************************************
 * ドライバーの初期化　及び　削除　他
 **********************************************************/
// ドライバーの初期化
static int sevenseg_probe(struct i2c_client *client) {
    u32 result;
    struct sevenseg_device_info *dev_info;

    dev_info = (struct sevenseg_device_info*)devm_kzalloc(&client->dev, sizeof(struct sevenseg_device_info), GFP_KERNEL);
    if (!dev_info) {
        pr_alert("%s(%s): Fail allocate memory for device_info\n", THIS_MODULE->name, __func__);
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
        
    // udevの生成
    result = make_udev(dev_info, THIS_MODULE->name);
    if (result != 0) goto err_udev;
    
    pr_info("%s(%s): Seven Segment LED driver installed. i2c_addr=[0x%02X]\n",
            THIS_MODULE->name, __func__, client->addr);
    return result;

err_udev:
    remove_sysfs(dev_info);
err:
    return result;
}

// ドライバの後始末
static int sevenseg_remove(struct i2c_client *client) {
    struct sevenseg_device_info *dev_info = (struct sevenseg_device_info *)i2c_get_clientdata(client);

    remove_udev(dev_info);
    remove_sysfs(dev_info);
    sevenseg_chip_off(dev_info);
    
    pr_info("%s(%s): Seven Segment LED driver removed.", THIS_MODULE->name, __func__);
    return 0;

}

// ドライバのシステムへの登録に関わる諸々
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
