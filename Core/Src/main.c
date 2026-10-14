/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx_hal.h"


/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>


/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c2;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
extern I2C_HandleTypeDef hi2c1;
extern const char* ALERT_PHONE;   // sadece bildirim
extern const char* ALERT_TEXT;


#define LCD_I2C_ADDR_0x27   (0x27 << 1)
#define LCD_I2C_ADDR_0x3F   (0x3F << 1)
#define LCD_ADDR            LCD_I2C_ADDR_0x27


#define ACTIVE_LOW          0

#define HOLD_TIME_MS        1500
#define QUIET_TO_SAFE_MS    500


#define LCD_BACKLIGHT 0x08
#define LCD_EN        0x04
#define LCD_RW        0x02
#define LCD_RS        0x01

static uint8_t lcd_backlight = LCD_BACKLIGHT;

static HAL_StatusTypeDef pcf8574_write(uint8_t data) {
    return HAL_I2C_Master_Transmit(&hi2c2, LCD_ADDR, &data, 1, 20);
}
static void lcd_pulse(uint8_t data) {
    pcf8574_write(data | LCD_EN | lcd_backlight);
    HAL_Delay(1);
    pcf8574_write((data & ~LCD_EN) | lcd_backlight);
    HAL_Delay(1);
}
static void lcd_write4(uint8_t nibble, uint8_t rs) {
    uint8_t d = (nibble & 0xF0) | (rs ? LCD_RS : 0x00);
    pcf8574_write(d | lcd_backlight);
    lcd_pulse(d);
}
static void lcd_send(uint8_t val, uint8_t rs) {
    lcd_write4(val & 0xF0, rs);
    lcd_write4((val << 4) & 0xF0, rs);
}
static void lcd_cmd(uint8_t cmd) { lcd_send(cmd, 0); HAL_Delay(2); }
static void lcd_data(uint8_t dt) { lcd_send(dt, 1); }
static void lcd_clear(void) { lcd_cmd(0x01); HAL_Delay(2); }
static void lcd_set_cursor(uint8_t c, uint8_t r) {
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    lcd_cmd(0x80 | (c + row_offsets[r]));
}
static void lcd_print(const char *s) { while (*s) lcd_data((uint8_t)*s++); }
static void lcd_init(void) {
    HAL_Delay(50);
    lcd_write4(0x30,0); HAL_Delay(5);
    lcd_write4(0x30,0); HAL_Delay(5);
    lcd_write4(0x30,0); HAL_Delay(5);
    lcd_write4(0x20,0); HAL_Delay(5); // 4-bit
    lcd_cmd(0x28); // 4-bit, 2 satır
    lcd_cmd(0x0C); // display on, cursor off
    lcd_clear();
    lcd_cmd(0x06); // entry mode
}

/* ---- Uygulama durumu ---- */
static inline uint32_t millis(void){ return HAL_GetTick(); }
volatile uint32_t lastPulseMs = 0;
uint8_t alarmShown = 0;
uint32_t alarmUntil = 0, quietAccum = 0;

/* Kısa yardımcılar: LED/Buzzer */
static void LED_On(void)  { HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET); }
static void LED_Off(void) { HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);   }
static void buzzer_on(void)  { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); }
static void buzzer_off(void) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); }
HAL_StatusTypeDef GSM_SendSMS(const char* number, const char* text);
static void alarmOn(void)
{
    if (!alarmShown)
    {
        alarmShown = 1;
        lcd_set_cursor(0,0);
        lcd_print("DEPREM OLUYOR!");

        GSM_SendSMS(ALERT_PHONE, ALERT_TEXT);  // <-- SMS sadece buradan gidecek
    }

    LED_On();
    buzzer_on();
}


static void alarmOff(void) {
    if (alarmShown) {  alarmShown = 0; }
    lcd_set_cursor(0,0); lcd_print("Guvenli Mod  ");

    LED_Off();
    buzzer_off();
}

uint32_t btn_msg_time = 0;
char btn_msg[17] = "                ";
// --- MENÜ DURUM MAKİNESİ ---
typedef enum {
    MENU_BOOT,
    MENU_MAIN,
    MENU_START,
    MENU_SETTINGS
} MenuState;

MenuState currentMenu = MENU_BOOT;

// --- MENÜ SEÇİM DEĞİŞKENLERİ ---
uint8_t menuMainSelection = 0;      // 0 = Start, 1 = Settings
uint8_t menuSettingsSelection = 0;  // 0 = Numara1, 1 = Numara2
uint8_t activeNumber = 0;           // GSM'e mesaj gidecek numara
// GSM için kullanılacak telefon numaraları

const char* ALERT_PHONE = "+90XXXXXXXXXX";   // <-- kendi telefon numaranizi yazin
const char* ALERT_TEXT  = "Deprem algilandi!";

uint32_t lastSmsMs = 0;                 // Son SMS gönderim zamanı
const uint32_t SMS_COOLDOWN_MS = 60000; // 60 saniye bekleme süresi
 // 60 saniye bekleme süresi

// --- BUTON EVENT FLAGLERİ ---
uint8_t up_pressed = 0;
uint8_t down_pressed = 0;
uint8_t enter_pressed = 0;
uint8_t back_pressed = 0;

    // --- POLLING buton durumları ---
    uint8_t up_prev=0, down_prev=0, enter_prev=0, back_prev=0;
    uint32_t up_last=0, down_last=0, enter_last=0, back_last=0;
    const uint32_t DEBOUNCE_MS = 120;   // çift basmayı önler
    extern UART_HandleTypeDef huart1;   // Eğer zaten yoksa ekle
    uint8_t gsm_connected = 0;
    char gsm_resp[32];






/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
void Menu_HandleButtons(void);
void Menu_Draw(void);



/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C2_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  lcd_init();
  alarmOff();        // açılış ekranı
  buzzer_off();      // emniyet
  LED_Off();



  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /* USER CODE BEGIN WHILE */
  uint32_t last = millis();
  while (1)
  {
      uint32_t now = millis();
      uint32_t dt  = now - last; if (dt > 20) dt = 20; last = now;


      uint32_t sincePulse = now - lastPulseMs;

      if (sincePulse < QUIET_TO_SAFE_MS) {
          alarmOn();
          alarmUntil = now + HOLD_TIME_MS;
          quietAccum = 0;
      } else {
          quietAccum += dt;
      }

      if ((now > alarmUntil) && (quietAccum >= QUIET_TO_SAFE_MS)) {
          alarmOff();
      }

      HAL_Delay(2);



      // ===== BUTON POLLING (INPUT) BURAYA =====
      // ===== BUTON POLLING (INPUT) =====
      uint32_t tnow = millis();

      // anlık okumalar
      uint8_t up_now    = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_2);
      uint8_t down_now  = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_3);
      uint8_t enter_now = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4);
      uint8_t back_now  = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6);

      // RISING edge + debounce -> event flag set et
      if(up_now && !up_prev && (tnow - up_last > DEBOUNCE_MS)){
          up_last = tnow;
          up_pressed = 1;
      }
      if(down_now && !down_prev && (tnow - down_last > DEBOUNCE_MS)){
          down_last = tnow;
          down_pressed = 1;
      }
      if(enter_now && !enter_prev && (tnow - enter_last > DEBOUNCE_MS)){
          enter_last = tnow;
          enter_pressed = 1;
      }
      if(back_now && !back_prev && (tnow - back_last > DEBOUNCE_MS)){
          back_last = tnow;
          back_pressed = 1;
      }

      // prev güncelle
      up_prev    = up_now;
      down_prev  = down_now;
      enter_prev = enter_now;
      back_prev  = back_now;

      // 2. satıra artık test yazısı yazmıyoruz — menü sistemi burayı yönetecek
      Menu_HandleButtons();
      Menu_Draw();
      HAL_Delay(2);
  }

  }



    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  /* USER CODE END 3 */


/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.ClockSpeed = 100000;
  hi2c2.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : VIB_SENSOR_Pin */
  GPIO_InitStruct.Pin = VIB_SENSOR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(VIB_SENSOR_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PA2 PA3 PA4 PA6 */
  GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PA5 */
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0){
        lastPulseMs = millis();
    }

}
void Menu_HandleButtons(void)
{
    // Event flag'leri lokal değişkene alalım (tek turda kullanıp sıfırlayacağız)
    uint8_t up    = up_pressed;
    uint8_t down  = down_pressed;
    uint8_t enter = enter_pressed;
    uint8_t back  = back_pressed;

    // Event'leri tükettiğimiz için flag'leri sıfırlıyoruz
    up_pressed = 0;
    down_pressed = 0;
    enter_pressed = 0;
    back_pressed = 0;

    switch(currentMenu)
    {
        case MENU_BOOT:
            // Açılış ekranı: herhangi bir tuşa basılırsa menüye geç
            if (up || down || enter || back) {
                currentMenu = MENU_MAIN;
                menuMainSelection = 0;  // Start seçili gelsin
            }
            break;

        case MENU_MAIN:
            // UP / DOWN ile Start <-> Settings arasında geç
            if (up || down) {
                if (menuMainSelection == 0)
                    menuMainSelection = 1;   // Settings
                else
                    menuMainSelection = 0;   // Start
            }

            // ENTER ile seçili menüye gir
            if (enter) {
                if (menuMainSelection == 0) {
                    // Start seçili -> deprem ekranına dön
                    currentMenu = MENU_START;
                } else {
                    // Settings seçili -> numara seçim ekranına gir
                    currentMenu = MENU_SETTINGS;
                }
            }

            // BACK ile açılış ekranına geri dön
            if (back) {
                currentMenu = MENU_BOOT;
            }
            break;

        case MENU_SETTINGS:
        	// UP -> bir üst numaraya
        	    if (up_pressed)
        	    {
        	        up_pressed = 0;
        	        if (menuSettingsSelection > 0)
        	            menuSettingsSelection--;
        	    }

        	    // DOWN -> bir alt numaraya
        	    if (down_pressed)
        	    {
        	        down_pressed = 0;
        	        if (menuSettingsSelection < 1)
        	            menuSettingsSelection++;
        	    }

        	    // ENTER -> seçili numarayı aktif yap ve KISA SÜRELİ "seçildi" yaz
        	    if (enter_pressed)
        	    {
        	        enter_pressed = 0;

        	        // Seçili numarayı aktif numara yap
        	        activeNumber = menuSettingsSelection;

        	        // LCD'yi temizleyip SEÇİLDİ mesajını göster
        	        lcd_clear();
        	        lcd_set_cursor(0,0);
        	        lcd_print("Numara ");
        	        if (activeNumber == 0)
        	            lcd_print("1 secildi");
        	        else
        	            lcd_print("2 secildi");

        	        // 0.8 saniye görünsün
        	        HAL_Delay(800);

        	        // Sonra ana menüye dön
        	        currentMenu = MENU_MAIN;
        	    }

        	    // BACK -> direkt ana menüye dön
        	    if (back_pressed)
        	    {
        	        back_pressed = 0;
        	        currentMenu = MENU_MAIN;
        	    }
            break;

        case MENU_START:
            // Deprem ekranı aktif, sadece BACK ile ana menüye dönebilelim
            if (back)
            {
                back = 0;              // bayrağı sıfırla
                currentMenu = MENU_MAIN;
            }

            // Burada artık ENTER ile SMS gönderme yok.
            // SMS tetiklemesi sadece sensör kodunda (alarmOn içinde) olacak.

            break;

    }
}

void Menu_Draw(void)
{
    // Sadece 2. satırı yönetiyoruz. 1. satır deprem kodunda.
    lcd_set_cursor(0,1);

    switch(currentMenu)
    {
    case MENU_BOOT:



        if (gsm_connected)
        {
            lcd_print("GSM Connected ");   // bağlı
        }
        else
        {
            lcd_print("GSM Kapali   ");    // bağlı değil
        }

        break;


        case MENU_MAIN:
            // Ana menü: Start / Settings
            if (menuMainSelection == 0) {
                // Start seçili
                lcd_print(">Start  Settings");
            } else {
                // Settings seçili
                lcd_print(" Start >Settings");
            }
            break;

        case MENU_SETTINGS:
            // Numara seçim ekranı (şimdilik Num1 / Num2)
            if (menuSettingsSelection == 0) {
                // Numara1 seçili
                lcd_print(">Num1   Num2   ");
            } else {
                // Numara2 seçili
                lcd_print(" Num1  >Num2   ");
            }
            break;

        case MENU_START:
            // Deprem ekranı aktifken 2. satır
            lcd_print("Deprem Modu    ");
            break;
    }
}
HAL_StatusTypeDef GSM_SendSMS(const char* number, const char* text)
{
	char cmd[64];

	    // 1) Text mode'a geç
	    const char* setTextMode = "AT+CMGF=1\r\n";
	    HAL_UART_Transmit(&huart1, (uint8_t*)setTextMode, strlen(setTextMode), 1000);
	    HAL_Delay(300);

	    // 2) Hangi numaraya SMS gidecek?
	    int len = snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", number);
	    if (len <= 0) {
	        return HAL_ERROR;
	    }
	    HAL_UART_Transmit(&huart1, (uint8_t*)cmd, len, 1000);
	    HAL_Delay(300);

	    // 3) Mesaj içeriği
	    HAL_UART_Transmit(&huart1, (uint8_t*)text, strlen(text), 1000);

	    // 4) Ctrl+Z (0x1A) ile SMS’i gönder
	    uint8_t ctrlZ = 26;
	    HAL_UART_Transmit(&huart1, &ctrlZ, 1, 1000);

	    // Modüle zaman tanıyalım (kaba ama iş görür)
	    HAL_Delay(5000);

	    return HAL_OK;
}

/* USER CODE BEGIN 4 */

/* USER CODE BEGIN 4 */

// Basit AT testi yapan fonksiyon


/* USER CODE END 4 */


/* USER CODE END 4 */


/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
