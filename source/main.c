// main.c - 3DS devkitPro tapping game (libctru + citro2d)
//
// Requires: libctru, citro2d, citro3d
// Make sure your project links citro2d/citro3d (most devkitPro 3ds templates do).
//
// Controls:
//  - A: Start Mode A (Normal)
//  - B: Start Mode B (Hard)
//  - SELECT: Exit
// Gameplay:
//  - Blue dots fall on the bottom screen (pink background)
//  - Tap dots to score +10
//  - Missed dot (goes off bottom) = lose 1 life
//  - 0 lives => GAME OVER for 3 seconds, then back to title

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ndsp.h>
#include <sys/stat.h>


#define TOP_W  400
#define TOP_H  240
#define BOT_W  320
#define BOT_H  240

#define MAX_DOTS 8

// Dot tuning
#define DOT_RADIUS 18.0f
#define DOT_SPAWN_Y    (-20.0f)

#define STREAM_CHUNK (64*1024)

static bool gResultsBuilt = false;
static FILE* gMusicFile = NULL;
static ndspWaveBuf gBuf[2];
static u8* gBufData[2];
static int gCurBuf = 0;

static int gBytesPerFrame = 4;   // will be set in openMusic()
static long gPcmStart = 44;      // where PCM starts (we assume 44 for now)
static C2D_TextBuf gUiBuf;
static C2D_TextBuf gDynBuf;

typedef enum {
    STATE_TITLE = 0,
    STATE_PLAY  = 1,
    STATE_GAMEOVER = 2,
    STATE_CREDITS = 3,
    STATE_RESULTS = 4
} GameState;



typedef enum {
    MODE_A = 0,
    MODE_B = 1
} GameMode;

typedef struct {
    bool active;
    float x, y;
    float vy; // pixels/sec
} Dot;

static GameState gState = STATE_TITLE;
static GameMode  gMode  = MODE_A;

static Dot gDots[MAX_DOTS];

static int gScore = 0;
static int gLives = 5;

static int gLastRoundScore = 0;
static int gHighScore = 0;


static C2D_TextBuf gScoreBuf = NULL;
static C2D_TextBuf gResultsBuf = NULL;
static C2D_Text gScoreText;



ndspWaveBuf popBuf;
u8* popData = NULL;
u32 popSize = 0;


ndspWaveBuf waveBuf;
u8* audioBuffer;
u32 audioSize;
u32 gWavRate = 44100;




// timers
static float gSpawnTimer = 0.0f;   // seconds since last spawn
static float gGameOverTimer = 0.0f;

// helpers
static float frandf(float a, float b) {
    return a + (b - a) * (float)rand() / (float)RAND_MAX;
}

static void resetDots(void) {
    for (int i = 0; i < MAX_DOTS; i++) {
        gDots[i].active = false;
        gDots[i].x = 0.0f;
        gDots[i].y = 0.0f;
        gDots[i].vy = 0.0f;
    }
}

static void startGame(GameMode mode) {
    gMode = mode;
    gScore = 0;
    gLives = 5;
    gSpawnTimer = 0.0f;
    gGameOverTimer = 0.0f;
    resetDots();
    gState = STATE_PLAY;


}





// Try to spawn a dot if there is a free slot.
// Speed/spawn gets harder as score increases.
static void trySpawnDot(void) {
    // Count active
    int activeCount = 0;
    for (int i = 0; i < MAX_DOTS; i++) if (gDots[i].active) activeCount++;
    if (activeCount >= MAX_DOTS) return;

    // Mode tuning
   const float baseSpawn = (gMode == MODE_A) ? 1.10f : 0.80f;   // more breathing room
   const float baseSpeed = (gMode == MODE_A) ? 45.0f : 65.0f;  // much slower start


    // Difficulty increases with score
    // Every 100 points: faster + slightly faster spawns
    float difficulty = (float)gScore / 200.0f;


    float spawnInterval = baseSpawn - 0.04f * difficulty;
    if (spawnInterval < 0.25f) spawnInterval = 0.25f;

    // Only spawn when timer passes interval
    if (gSpawnTimer < spawnInterval) return;

    // Find free slot
    for (int i = 0; i < MAX_DOTS; i++) {
        if (!gDots[i].active) {
            gDots[i].active = true;
            gDots[i].x = frandf(DOT_RADIUS + 6.0f, (float)BOT_W - DOT_RADIUS - 6.0f);
            gDots[i].y = DOT_SPAWN_Y;

            float speed = baseSpeed + 12.0f * difficulty;
            if (speed > 190.0f) speed = 190.0f;
            if (speed > 190.0f) speed = 190.0f;
            gDots[i].vy = speed;

            gSpawnTimer = 0.0f;
            break;
        }
    }
}

static bool pointInDot(float px, float py, const Dot* d) {
    float dx = px - d->x;
    float dy = py - d->y;
    float r = DOT_RADIUS;
    return (dx*dx + dy*dy) <= (r*r);
}
static u16 rd16(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }


static bool musicOpen(const char* path)
{
    gMusicFile = fopen(path, "rb");
    if (!gMusicFile) return false;


    unsigned char hdr[44];
    fread(hdr, 1, 44, gMusicFile);

    u16 channels = hdr[22] | (hdr[23] << 8);
    u32 rate     = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    u16 bits     = hdr[34] | (hdr[35] << 8);

    if (bits != 16 || (channels != 1 && channels != 2)) return false;

    gBytesPerFrame = channels * 2;

    ndspChnSetRate(0, (float)rate);
    ndspChnSetFormat(0, channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                                      : NDSP_FORMAT_MONO_PCM16);

    fseek(gMusicFile, gPcmStart, SEEK_SET);

    for(int i=0;i<2;i++){
       gBufData[i] = linearAlloc(STREAM_CHUNK);
        memset(&gBuf[i],0,sizeof(ndspWaveBuf));
        gBuf[i].status = NDSP_WBUF_FREE;
    }

    return true;
}

static void musicUpdate(void)
{
    ndspWaveBuf* b = &gBuf[gCurBuf];

    if (b->status == NDSP_WBUF_DONE) b->status = NDSP_WBUF_FREE;

    if (b->status == NDSP_WBUF_FREE) {
        size_t r = fread(gBufData[gCurBuf],1,STREAM_CHUNK,gMusicFile);

        if (r == 0) { fseek(gMusicFile, gPcmStart, SEEK_SET); return; }

      DSP_FlushDataCache(gBufData[gCurBuf], r);
       b->data_vaddr = gBufData[gCurBuf];
        b->nsamples   = r / gBytesPerFrame;
        ndspChnWaveBufAdd(0, b);
        gCurBuf ^= 1;
    }
}

static void musicPrime(void)
{
    musicUpdate();
    musicUpdate();
}

static void drawHeart(float x, float y, float size, u32 color)
{
    float r = size * 0.25f;

    // top bumps
    C2D_DrawCircleSolid(x - r, y, 0, r, color);
    C2D_DrawCircleSolid(x + r, y, 0, r, color);

    // bottom
    C2D_DrawRectSolid(x - size/2, y, 0, size, size/1.3f, color);
}

static void UpdateScoreText(int score)
{
    if (!gDynBuf) return;


    char scoreStr[32];
    snprintf(scoreStr, sizeof(scoreStr), "Score: %d", score);

   C2D_TextBufClear(gDynBuf);

    C2D_TextParse(&gScoreText, gDynBuf, scoreStr);

}


static void LoadPopSound(void)
{
    FILE* f = fopen("romfs:/pop.wav","rb");
    if(!f) return;

    fseek(f, 44, SEEK_SET);

    fseek(f, 0, SEEK_END);
    popSize = ftell(f) - 44;
    fseek(f, 44, SEEK_SET);

    popData = linearAlloc(popSize + 0x40);
    fread(popData,1,popSize,f);
    fclose(f);

    memset(&popBuf, 0, sizeof(ndspWaveBuf));
}

static void PlayPop(void)
{
    if (!popData) return;

    ndspChnWaveBufClear(1);

    popBuf.data_vaddr = popData;
    popBuf.nsamples = popSize / 2;
    popBuf.looping = false;
    popBuf.status = NDSP_WBUF_FREE;

    DSP_FlushDataCache(popData, popSize);
    ndspChnWaveBufAdd(1, &popBuf);
}



static void SaveHighScore(void)
{
    FILE* f = fopen("sdmc:/tapdots_highscore.dat", "w");
    if (f)
    {
        fprintf(f, "%d", gHighScore);
        fclose(f);
    }
}


static void LoadHighScore(void)
{
    FILE* f = fopen("sdmc:/tapdots_highscore.dat", "r");
    if (f)
    {
        fscanf(f, "%d", &gHighScore);
        fclose(f);
    }
    else
    {
        gHighScore = 0;
    }
}



int main(int argc, char* argv[]) {
    (void)argc; (void)argv;


    gfxInitDefault();
    consoleClear();

romfsInit();
LoadHighScore();
ndspInit();
ndspChnReset(0);
LoadPopSound();


float mix[12] = {0};
mix[0] = mix[1] = 1.0f;

/* MUSIC CHANNEL (0) */
ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
ndspChnSetRate(0, 44100.0f);
ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
ndspChnSetMix(0, mix);

/* SFX CHANNEL (1) */
ndspChnSetInterp(1, NDSP_INTERP_POLYPHASE);
ndspChnSetRate(1, 44100.0f);
ndspChnSetFormat(1, NDSP_FORMAT_MONO_PCM16);
ndspChnSetMix(1, mix);

ndspSetOutputMode(NDSP_OUTPUT_STEREO);
ndspSetMasterVol(1.0f);

if (musicOpen("romfs:/music.wav")) {
    musicPrime();
}
 

    // Basic init
    C2D_Image bgImage;
    C2D_Image heartImage;
    C2D_SpriteSheet heartSheet;


    consoleClear();


    // citro2d init
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
   gScoreBuf = C2D_TextBufNew(256);
   gResultsBuf = C2D_TextBufNew(256);


    C2D_Prepare();
    gUiBuf  = C2D_TextBufNew(4096);
    gDynBuf = C2D_TextBufNew(512);



C2D_Text titleText, menuText1, menuText2, menuText3;
C2D_Text finalText;
C2D_Text creditsTitle, creditsLine1, creditsLine2;
C2D_Text resultsTitle, lastScoreText, totalScoreText;


C2D_TextParse(&resultsTitle, gUiBuf, "RESULTS");

C2D_TextParse(&titleText, gUiBuf, "Tap Dots");
C2D_TextParse(&menuText1, gUiBuf, "Press A - Normal");
C2D_TextParse(&menuText2, gUiBuf, "Press B - Hard");
C2D_TextParse(&menuText3, gUiBuf, "Press X - Credits");
C2D_TextParse(&finalText, gUiBuf, "GAME OVER");

C2D_TextParse(&creditsTitle, gUiBuf, "CREDITS");
C2D_TextParse(&creditsLine1, gUiBuf, "Most of the game by: Isa26");
C2D_TextParse(&creditsLine2, gUiBuf, "Music by: Kevin MacLeod");

    bgImage = C2D_SpriteSheetGetImage(C2D_SpriteSheetLoad("romfs:/gfx/bg.t3x"), 0);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    heartSheet = C2D_SpriteSheetLoad("romfs:/gfx/heart.t3x");
    heartImage = C2D_SpriteSheetGetImage(heartSheet, 0);

    // Create render targets
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);



    // Seed RNG
    srand((unsigned)osGetTime());

    UpdateScoreText(gScore);




    // Colors
    const u32 COLOR_PINK = C2D_Color32(255, 120, 200, 255);
    const u32 COLOR_BLUE = C2D_Color32(40, 120, 255, 255);
    const u32 COLOR_WHITE = C2D_Color32(255, 255, 255, 255);
    const u32 COLOR_BLACK = C2D_Color32(0, 0, 0, 255);
    
    


    // Font (citro2d has a basic system font wrapper)
    // Using C2D_Text for nicer top-screen text.

 resetDots();

u64 lastTick = svcGetSystemTick();
const double tickFreq = SYSCLOCK_ARM11;

while (aptMainLoop())
{
    hidScanInput();
    musicUpdate();

    u32 kDown = hidKeysDown();
    if (kDown & KEY_SELECT) break;

    u64 now = svcGetSystemTick();
    double dt = (double)(now - lastTick) / tickFreq;
    lastTick = now;
    if (dt > 0.05) dt = 0.05;

    touchPosition touch;
    hidTouchRead(&touch);
    bool touched = (kDown & KEY_TOUCH);

    /* STATE LOGIC */
    if (gState == STATE_TITLE)
    {
        if (kDown & KEY_A) { startGame(MODE_A);UpdateScoreText(gScore); }
        if (kDown & KEY_B) { startGame(MODE_B); UpdateScoreText(gScore); }
        if (kDown & KEY_X) gState = STATE_CREDITS;
        if(kDown & KEY_Y) gState = STATE_RESULTS;

    }
    else if (gState == STATE_CREDITS)
    {
        if (kDown & (KEY_A|KEY_B|KEY_X|KEY_START)) gState = STATE_TITLE;


    }
    else if (gState == STATE_PLAY)
    {
        gSpawnTimer += dt;
        trySpawnDot();

        for (int i=0;i<MAX_DOTS;i++)
        {
            if(!gDots[i].active) continue;
            gDots[i].y += gDots[i].vy * dt;

            if (gDots[i].y - DOT_RADIUS > BOT_H)
            {
                gDots[i].active = false;
                if(--gLives <= 0)
                {
                    gLives = 0;
                    gState = STATE_GAMEOVER;
                    gGameOverTimer = 0;
                }
            }
        }

    if (touched)
{
    float tx = touch.px;
    float ty = touch.py;

    for (int i=0;i<MAX_DOTS;i++)
    {
   if(gDots[i].active && pointInDot(tx,ty,&gDots[i]))
{
    PlayPop();           // 🔊 now it fires
    gDots[i].active = false;
    gScore += 10;
    UpdateScoreText(gScore);
    break;
}

        }
     }
   }

else if (gState == STATE_GAMEOVER)
{
    gGameOverTimer += dt;

if (gGameOverTimer >= 3.0f)
{
    gLastRoundScore = gScore;

   if (gScore > gHighScore)
{
    gHighScore = gScore;
    SaveHighScore();
}


    gState = STATE_RESULTS;
    gResultsBuilt = false;
    gGameOverTimer = -9999.0f;
}

}


C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

/* TOP */
C2D_TargetClear(top,C2D_Color32(0,0,0,255));
C2D_SceneBegin(top);
C2D_DrawImageAt(bgImage,0,0,0,NULL,1,1);

C2D_DrawText(&titleText,C2D_AlignCenter,200,20,0,0.7f,0.7f,COLOR_WHITE);

if(gState==STATE_TITLE)
{
    C2D_DrawText(&menuText1,C2D_AlignCenter,200,90,0,0.5f,0.5f,COLOR_WHITE);
    C2D_DrawText(&menuText2,C2D_AlignCenter,200,120,0,0.5f,0.5f,COLOR_WHITE);
    C2D_DrawText(&menuText3,C2D_AlignCenter,200,150,0,0.5f,0.5f,COLOR_WHITE);
}
else if(gState==STATE_CREDITS)
{
    C2D_DrawText(&creditsTitle,C2D_AlignCenter,200,60,0,0.7f,0.7f,COLOR_WHITE);
    C2D_DrawText(&creditsLine1,C2D_AlignCenter,200,120,0,0.5f,0.5f,COLOR_WHITE);
    C2D_DrawText(&creditsLine2,C2D_AlignCenter,200,150,0,0.5f,0.5f,COLOR_WHITE);
}

else if(gState == STATE_RESULTS)
{
    if(!gResultsBuilt)
    {
        char buf[64];
        C2D_TextBufClear(gResultsBuf);


        sprintf(buf,"Last Round: %d", gLastRoundScore);
        C2D_TextParse(&lastScoreText, gResultsBuf, buf);

       sprintf(buf,"HIGH SCORE: %d", gHighScore);
       C2D_TextParse(&totalScoreText, gResultsBuf, buf);


        gResultsBuilt = true;
    }

    if (kDown & (KEY_A|KEY_B|KEY_START))
        gState = STATE_TITLE;

    C2D_DrawText(&resultsTitle,C2D_AlignCenter,200,40,0,0.7f,0.7f,COLOR_WHITE);
    C2D_DrawText(&lastScoreText,C2D_AlignCenter,200,110,0,0.6f,0.6f,COLOR_WHITE);
    C2D_DrawText(&totalScoreText,C2D_AlignCenter,200,150,0,0.6f,0.6f,COLOR_WHITE);
}


else if(gState==STATE_PLAY)
{
   C2D_DrawText(&gScoreText, 0,10,10,0,0.6f,0.6f,COLOR_WHITE);



    float scoreX = 10;
    float scoreY = 10;

    for (int i = 0; i < gLives; i++)
    {
        C2D_DrawImageAt(
            heartImage,
            scoreX + i * 22,
            scoreY + 26,
            0,
            NULL,
            1.0f,
            1.0f
        );
    }
}   // <<< THIS brace is missing in your file


    /* BOTTOM */
    C2D_TargetClear(bot,COLOR_PINK);
    C2D_SceneBegin(bot);
    for(int i=0;i<MAX_DOTS;i++)
        if(gDots[i].active)
            C2D_DrawCircleSolid(gDots[i].x,gDots[i].y,0,DOT_RADIUS,COLOR_BLUE);

    if(gState==STATE_GAMEOVER)
        C2D_DrawText(&finalText,C2D_AlignCenter,160,120,0,1.3f,1.3f,C2D_Color32(220,0,0,255));

    C3D_FrameEnd(0);
}

/* CLEANUP */
C2D_SpriteSheetFree(heartSheet);
C2D_TextBufDelete(gUiBuf);
C2D_TextBufDelete(gDynBuf);
C2D_TextBufDelete(gResultsBuf);
C2D_Fini();
ndspExit();
C3D_Fini();
SaveHighScore();
gfxExit();
return 0;
}
