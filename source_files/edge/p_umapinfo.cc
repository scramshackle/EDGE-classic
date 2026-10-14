//----------------------------------------------------------------------------
//  EDGE UMAPINFO Parsing Code
//----------------------------------------------------------------------------
//
//  Copyright (c) 2023-2024 The EDGE Team.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------------
//
//  Based on the UMAPINFO reference implementation, released by Christoph
//  Oelckers under the following copyright:
//
//  Copyright 2017 Christoph Oelckers
//
//----------------------------------------------------------------------------

#include "p_umapinfo.h"

#include <unordered_map> // ZDoom Actor Name <-> Dehacked/Doomednum lookups

#include "ddf_game.h"
#include "ddf_language.h"
#include "ddf_thing.h"
#include "deh_text.h"
#include "epi_scanner.h"
#include "epi_str_compare.h"
#include "epi_str_hash.h"
#include "epi_str_util.h"
#include "i_system.h"

MapList Maps;

namespace umapinfo
{

constexpr uint32_t kLevelName             = epi::KnownStringHash("LEVELNAME");
constexpr uint32_t kLabel                 = epi::KnownStringHash("LABEL");
constexpr uint32_t kNext                  = epi::KnownStringHash("NEXT");
constexpr uint32_t kNextSecret            = epi::KnownStringHash("NEXTSECRET");
constexpr uint32_t kLevelPic              = epi::KnownStringHash("LEVELPIC");
constexpr uint32_t kSkyTexture            = epi::KnownStringHash("SKYTEXTURE");
constexpr uint32_t kMusic                 = epi::KnownStringHash("MUSIC");
constexpr uint32_t kEndPic                = epi::KnownStringHash("ENDPIC");
constexpr uint32_t kEndCast               = epi::KnownStringHash("ENDCAST");
constexpr uint32_t kEndBunny              = epi::KnownStringHash("ENDBUNNY");
constexpr uint32_t kEndGame               = epi::KnownStringHash("ENDGAME");
constexpr uint32_t kExitPic               = epi::KnownStringHash("EXITPIC");
constexpr uint32_t kEnterPic              = epi::KnownStringHash("ENTERPIC");
constexpr uint32_t kNoIntermission        = epi::KnownStringHash("NOINTERMISSION");
constexpr uint32_t kParTime               = epi::KnownStringHash("PARTIME");
constexpr uint32_t kIntertext             = epi::KnownStringHash("INTERTEXT");
constexpr uint32_t kIntertextSecret       = epi::KnownStringHash("INTERTEXTSECRET");
constexpr uint32_t kInterbackdrop         = epi::KnownStringHash("INTERBACKDROP");
constexpr uint32_t kIntermusic            = epi::KnownStringHash("INTERMUSIC");
constexpr uint32_t kEpisode               = epi::KnownStringHash("EPISODE");
constexpr uint32_t kBossAction            = epi::KnownStringHash("BOSSACTION");
constexpr uint32_t kAuthor                = epi::KnownStringHash("AUTHOR");
constexpr uint32_t kDoomPlayer            = epi::KnownStringHash("DOOMPLAYER");
constexpr uint32_t kZombieMan             = epi::KnownStringHash("ZOMBIEMAN");
constexpr uint32_t kShotgunGuy            = epi::KnownStringHash("SHOTGUNGUY");
constexpr uint32_t kArchvile              = epi::KnownStringHash("ARCHVILE");
constexpr uint32_t kArchvileFire          = epi::KnownStringHash("ARCHVILEFIRE");
constexpr uint32_t kRevenant              = epi::KnownStringHash("REVENANT");
constexpr uint32_t kRevenantTracer        = epi::KnownStringHash("REVENANTTRACER");
constexpr uint32_t kRevenantTracerSmoke   = epi::KnownStringHash("REVENANTTRACERSMOKE");
constexpr uint32_t kFatso                 = epi::KnownStringHash("FATSO");
constexpr uint32_t kFatShot               = epi::KnownStringHash("FATSHOT");
constexpr uint32_t kChaingunGuy           = epi::KnownStringHash("CHAINGUNGUY");
constexpr uint32_t kDoomImp               = epi::KnownStringHash("DOOMIMP");
constexpr uint32_t kDemon                 = epi::KnownStringHash("DEMON");
constexpr uint32_t kSpectre               = epi::KnownStringHash("SPECTRE");
constexpr uint32_t kCacodemon             = epi::KnownStringHash("CACODEMON");
constexpr uint32_t kBaronOfHell           = epi::KnownStringHash("BARONOFHELL");
constexpr uint32_t kBaronBall             = epi::KnownStringHash("BARONBALL");
constexpr uint32_t kHellKnight            = epi::KnownStringHash("HELLKNIGHT");
constexpr uint32_t kLostSoul              = epi::KnownStringHash("LOSTSOUL");
constexpr uint32_t kSpiderMastermind      = epi::KnownStringHash("SPIDERMASTERMIND");
constexpr uint32_t kArachnotron           = epi::KnownStringHash("ARACHNOTRON");
constexpr uint32_t kCyberdemon            = epi::KnownStringHash("CYBERDEMON");
constexpr uint32_t kPainElemental         = epi::KnownStringHash("PAINELEMENTAL");
constexpr uint32_t kWolfensteinSS         = epi::KnownStringHash("WOLFENSTEINSS");
constexpr uint32_t kCommanderKeen         = epi::KnownStringHash("COMMANDERKEEN");
constexpr uint32_t kBossBrain             = epi::KnownStringHash("BOSSBRAIN");
constexpr uint32_t kBossEye               = epi::KnownStringHash("BOSSEYE");
constexpr uint32_t kBossTarget            = epi::KnownStringHash("BOSSTARGET");
constexpr uint32_t kSpawnShot             = epi::KnownStringHash("SPAWNSHOT");
constexpr uint32_t kSpawnFire             = epi::KnownStringHash("SPAWNFIRE");
constexpr uint32_t kExplosiveBarrel       = epi::KnownStringHash("EXPLOSIVEBARREL");
constexpr uint32_t kDoomImpBall           = epi::KnownStringHash("DOOMIMPBALL");
constexpr uint32_t kCacodemonBall         = epi::KnownStringHash("CACODEMONBALL");
constexpr uint32_t kRocket                = epi::KnownStringHash("ROCKET");
constexpr uint32_t kPlasmaBall            = epi::KnownStringHash("PLASMABALL");
constexpr uint32_t kBFGBall               = epi::KnownStringHash("BFGBALL");
constexpr uint32_t kArachnotronPlasma     = epi::KnownStringHash("ARACHNOTRONPLASMA");
constexpr uint32_t kBulletPuff            = epi::KnownStringHash("BULLETPUFF");
constexpr uint32_t kBlood                 = epi::KnownStringHash("BLOOD");
constexpr uint32_t kTeleportFog           = epi::KnownStringHash("TELEPORTFOG");
constexpr uint32_t kItemFog               = epi::KnownStringHash("ITEMFOG");
constexpr uint32_t kTeleportDest          = epi::KnownStringHash("TELEPORTDEST");
constexpr uint32_t kBFGExtra              = epi::KnownStringHash("BFGEXTRA");
constexpr uint32_t kGreenArmor            = epi::KnownStringHash("GREENARMOR");
constexpr uint32_t kBlueArmor             = epi::KnownStringHash("BLUEARMOR");
constexpr uint32_t kHealthBonus           = epi::KnownStringHash("HEALTHBONUS");
constexpr uint32_t kArmorBonus            = epi::KnownStringHash("ARMORBONUS");
constexpr uint32_t kBlueCard              = epi::KnownStringHash("BLUECARD");
constexpr uint32_t kRedCard               = epi::KnownStringHash("REDCARD");
constexpr uint32_t kYellowCard            = epi::KnownStringHash("YELLOWCARD");
constexpr uint32_t kYellowSkull           = epi::KnownStringHash("YELLOWSKULL");
constexpr uint32_t kRedSkull              = epi::KnownStringHash("REDSKULL");
constexpr uint32_t kBlueSkull             = epi::KnownStringHash("BLUESKULL");
constexpr uint32_t kStimpack              = epi::KnownStringHash("STIMPACK");
constexpr uint32_t kMedikit               = epi::KnownStringHash("MEDIKIT");
constexpr uint32_t kSoulsphere            = epi::KnownStringHash("SOULSPHERE");
constexpr uint32_t kInvulnerabilitySphere = epi::KnownStringHash("INVULNERABILITYSPHERE");
constexpr uint32_t kBerserk               = epi::KnownStringHash("BERSERK");
constexpr uint32_t kBlurSphere            = epi::KnownStringHash("BLURSPHERE");
constexpr uint32_t kRadSuit               = epi::KnownStringHash("RADSUIT");
constexpr uint32_t kAllmap                = epi::KnownStringHash("ALLMAP");
constexpr uint32_t kInfrared              = epi::KnownStringHash("INFRARED");
constexpr uint32_t kMegasphere            = epi::KnownStringHash("MEGASPHERE");
constexpr uint32_t kClip                  = epi::KnownStringHash("CLIP");
constexpr uint32_t kClipBox               = epi::KnownStringHash("CLIPBOX");
constexpr uint32_t kRocketAmmo            = epi::KnownStringHash("ROCKETAMMO");
constexpr uint32_t kRocketBox             = epi::KnownStringHash("ROCKETBOX");
constexpr uint32_t kCell                  = epi::KnownStringHash("CELL");
constexpr uint32_t kCellPack              = epi::KnownStringHash("CELLPACK");
constexpr uint32_t kShell                 = epi::KnownStringHash("SHELL");
constexpr uint32_t kShellBox              = epi::KnownStringHash("SHELLBOX");
constexpr uint32_t kBackpack              = epi::KnownStringHash("BACKPACK");
constexpr uint32_t kBFG9000               = epi::KnownStringHash("BFG9000");
constexpr uint32_t kChaingun              = epi::KnownStringHash("CHAINGUN");
constexpr uint32_t kChainsaw              = epi::KnownStringHash("CHAINSAW");
constexpr uint32_t kRocketLauncher        = epi::KnownStringHash("ROCKETLAUNCHER");
constexpr uint32_t kPlasmaRifle           = epi::KnownStringHash("PLASMARIFLE");
constexpr uint32_t kShotgun               = epi::KnownStringHash("SHOTGUN");
constexpr uint32_t kSuperShotgun          = epi::KnownStringHash("SUPERSHOTGUN");
constexpr uint32_t kTechLamp              = epi::KnownStringHash("TECHLAMP");
constexpr uint32_t kTechLamp2             = epi::KnownStringHash("TECHLAMP2");
constexpr uint32_t kColumn                = epi::KnownStringHash("COLUMN");
constexpr uint32_t kTallGreenColumn       = epi::KnownStringHash("TALLGREENCOLUMN");
constexpr uint32_t kShortGreenColumn      = epi::KnownStringHash("SHORTGREENCOLUMN");
constexpr uint32_t kTallRedColumn         = epi::KnownStringHash("TALLREDCOLUMN");
constexpr uint32_t kShortRedColumn        = epi::KnownStringHash("SHORTREDCOLUMN");
constexpr uint32_t kSkullColumn           = epi::KnownStringHash("SKULLCOLUMN");
constexpr uint32_t kHeartColumn           = epi::KnownStringHash("HEARTCOLUMN");
constexpr uint32_t kEvilEye               = epi::KnownStringHash("EVILEYE");
constexpr uint32_t kFloatingSkull         = epi::KnownStringHash("FLOATINGSKULL");
constexpr uint32_t kTorchTree             = epi::KnownStringHash("TORCHTREE");
constexpr uint32_t kBlueTorch             = epi::KnownStringHash("BLUETORCH");
constexpr uint32_t kGreenTorch            = epi::KnownStringHash("GREENTORCH");
constexpr uint32_t kRedTorch              = epi::KnownStringHash("REDTORCH");
constexpr uint32_t kShortBlueTorch        = epi::KnownStringHash("SHORTBLUETORCH");
constexpr uint32_t kShortGreenTorch       = epi::KnownStringHash("SHORTGREENTORCH");
constexpr uint32_t kShortRedTorch         = epi::KnownStringHash("SHORTREDTORCH");
constexpr uint32_t kStalagtite            = epi::KnownStringHash("STALAGTITE");
constexpr uint32_t kTechPillar            = epi::KnownStringHash("TECHPILLAR");
constexpr uint32_t kCandleStick           = epi::KnownStringHash("CANDLESTICK");
constexpr uint32_t kCandelabra            = epi::KnownStringHash("CANDELABRA");
constexpr uint32_t kBloodyTwitch          = epi::KnownStringHash("BLOODYTWITCH");
constexpr uint32_t kMeat2                 = epi::KnownStringHash("MEAT2");
constexpr uint32_t kMeat3                 = epi::KnownStringHash("MEAT3");
constexpr uint32_t kMeat4                 = epi::KnownStringHash("MEAT4");
constexpr uint32_t kMeat5                 = epi::KnownStringHash("MEAT5");
constexpr uint32_t kNonsolidMeat2         = epi::KnownStringHash("NONSOLIDMEAT2");
constexpr uint32_t kNonsolidMeat4         = epi::KnownStringHash("NONSOLIDMEAT4");
constexpr uint32_t kNonsolidMeat3         = epi::KnownStringHash("NONSOLIDMEAT3");
constexpr uint32_t kNonsolidMeat5         = epi::KnownStringHash("NONSOLIDMEAT5");
constexpr uint32_t kNonsolidTwitch        = epi::KnownStringHash("NONSOLIDTWITCH");
constexpr uint32_t kDeadCacodemon         = epi::KnownStringHash("DEADCACODEMON");
constexpr uint32_t kDeadMarine            = epi::KnownStringHash("DEADMARINE");
constexpr uint32_t kDeadZombieMan         = epi::KnownStringHash("DEADZOMBIEMAN");
constexpr uint32_t kDeadDemon             = epi::KnownStringHash("DEADDEMON");
constexpr uint32_t kDeadLostSoul          = epi::KnownStringHash("DEADLOSTSOUL");
constexpr uint32_t kDeadDoomImp           = epi::KnownStringHash("DEADDOOMIMP");
constexpr uint32_t kDeadShotgunGuy        = epi::KnownStringHash("DEADSHOTGUNGUY");
constexpr uint32_t kGibbedMarine          = epi::KnownStringHash("GIBBEDMARINE");
constexpr uint32_t kGibbedMarineExtra     = epi::KnownStringHash("GIBBEDMARINEEXTRA");
constexpr uint32_t kHeadsOnAStick         = epi::KnownStringHash("HEADSONASTICK");
constexpr uint32_t kGibs                  = epi::KnownStringHash("GIBS");
constexpr uint32_t kHeadOnAStick          = epi::KnownStringHash("HEADONASTICK");
constexpr uint32_t kHeadCandles           = epi::KnownStringHash("HEADCANDLES");
constexpr uint32_t kDeadStick             = epi::KnownStringHash("DEADSTICK");
constexpr uint32_t kLiveStick             = epi::KnownStringHash("LIVESTICK");
constexpr uint32_t kBigTree               = epi::KnownStringHash("BIGTREE");
constexpr uint32_t kBurningBarrel         = epi::KnownStringHash("BURNINGBARREL");
constexpr uint32_t kHangNoGuts            = epi::KnownStringHash("HANGNOGUTS");
constexpr uint32_t kHangBNoBrain          = epi::KnownStringHash("HANGBNOBRAIN");
constexpr uint32_t kHangTLookingDown      = epi::KnownStringHash("HANGTLOOKINGDOWN");
constexpr uint32_t kHangTSkull            = epi::KnownStringHash("HANGTSKULL");
constexpr uint32_t kHangTLookingUp        = epi::KnownStringHash("HANGTLOOKINGUP");
constexpr uint32_t kHangTNoBrain          = epi::KnownStringHash("HANGTNOBRAIN");
constexpr uint32_t kColonGibs             = epi::KnownStringHash("COLONGIBS");
constexpr uint32_t kSmallBloodPool        = epi::KnownStringHash("SMALLBLOODPOOL");
constexpr uint32_t kBrainStem             = epi::KnownStringHash("BRAINSTEM");
constexpr uint32_t kPointPusher           = epi::KnownStringHash("POINTPUSHER");
constexpr uint32_t kPointPuller           = epi::KnownStringHash("POINTPULLER");
constexpr uint32_t kMBFHelperDog          = epi::KnownStringHash("MBFHELPERDOG");
constexpr uint32_t kPlasmaBall1           = epi::KnownStringHash("PLASMABALL1");
constexpr uint32_t kPlasmaBall2           = epi::KnownStringHash("PLASMABALL2");
constexpr uint32_t kEvilSceptre           = epi::KnownStringHash("EVILSCEPTRE");
constexpr uint32_t kUnholyBible           = epi::KnownStringHash("UNHOLYBIBLE");
constexpr uint32_t kMusicChanger          = epi::KnownStringHash("MUSICCHANGER");
constexpr uint32_t kDehActor145           = epi::KnownStringHash("DEH_ACTOR_145");
constexpr uint32_t kDehActor146           = epi::KnownStringHash("DEH_ACTOR_146");
constexpr uint32_t kDehActor147           = epi::KnownStringHash("DEH_ACTOR_147");
constexpr uint32_t kDehActor148           = epi::KnownStringHash("DEH_ACTOR_148");
constexpr uint32_t kDehActor149           = epi::KnownStringHash("DEH_ACTOR_149");
constexpr uint32_t kDehActor150           = epi::KnownStringHash("DEH_ACTOR_150");
constexpr uint32_t kDehActor151           = epi::KnownStringHash("DEH_ACTOR_151");
constexpr uint32_t kDehActor152           = epi::KnownStringHash("DEH_ACTOR_152");
constexpr uint32_t kDehActor153           = epi::KnownStringHash("DEH_ACTOR_153");
constexpr uint32_t kDehActor154           = epi::KnownStringHash("DEH_ACTOR_154");
constexpr uint32_t kDehActor155           = epi::KnownStringHash("DEH_ACTOR_155");
constexpr uint32_t kDehActor156           = epi::KnownStringHash("DEH_ACTOR_156");
constexpr uint32_t kDehActor157           = epi::KnownStringHash("DEH_ACTOR_157");
constexpr uint32_t kDehActor158           = epi::KnownStringHash("DEH_ACTOR_158");
constexpr uint32_t kDehActor159           = epi::KnownStringHash("DEH_ACTOR_159");
constexpr uint32_t kDehActor160           = epi::KnownStringHash("DEH_ACTOR_160");
constexpr uint32_t kDehActor161           = epi::KnownStringHash("DEH_ACTOR_161");
constexpr uint32_t kDehActor162           = epi::KnownStringHash("DEH_ACTOR_162");
constexpr uint32_t kDehActor163           = epi::KnownStringHash("DEH_ACTOR_163");
constexpr uint32_t kDehActor164           = epi::KnownStringHash("DEH_ACTOR_164");
constexpr uint32_t kDehActor165           = epi::KnownStringHash("DEH_ACTOR_165");
constexpr uint32_t kDehActor166           = epi::KnownStringHash("DEH_ACTOR_166");
constexpr uint32_t kDehActor167           = epi::KnownStringHash("DEH_ACTOR_167");
constexpr uint32_t kDehActor168           = epi::KnownStringHash("DEH_ACTOR_168");
constexpr uint32_t kDehActor169           = epi::KnownStringHash("DEH_ACTOR_169");
constexpr uint32_t kDehActor170           = epi::KnownStringHash("DEH_ACTOR_170");
constexpr uint32_t kDehActor171           = epi::KnownStringHash("DEH_ACTOR_171");
constexpr uint32_t kDehActor172           = epi::KnownStringHash("DEH_ACTOR_172");
constexpr uint32_t kDehActor173           = epi::KnownStringHash("DEH_ACTOR_173");
constexpr uint32_t kDehActor174           = epi::KnownStringHash("DEH_ACTOR_174");
constexpr uint32_t kDehActor175           = epi::KnownStringHash("DEH_ACTOR_175");
constexpr uint32_t kDehActor176           = epi::KnownStringHash("DEH_ACTOR_176");
constexpr uint32_t kDehActor177           = epi::KnownStringHash("DEH_ACTOR_177");
constexpr uint32_t kDehActor178           = epi::KnownStringHash("DEH_ACTOR_178");
constexpr uint32_t kDehActor179           = epi::KnownStringHash("DEH_ACTOR_179");
constexpr uint32_t kDehActor180           = epi::KnownStringHash("DEH_ACTOR_180");
constexpr uint32_t kDehActor181           = epi::KnownStringHash("DEH_ACTOR_181");
constexpr uint32_t kDehActor182           = epi::KnownStringHash("DEH_ACTOR_182");
constexpr uint32_t kDehActor183           = epi::KnownStringHash("DEH_ACTOR_183");
constexpr uint32_t kDehActor184           = epi::KnownStringHash("DEH_ACTOR_184");
constexpr uint32_t kDehActor185           = epi::KnownStringHash("DEH_ACTOR_185");
constexpr uint32_t kDehActor186           = epi::KnownStringHash("DEH_ACTOR_186");
constexpr uint32_t kDehActor187           = epi::KnownStringHash("DEH_ACTOR_187");
constexpr uint32_t kDehActor188           = epi::KnownStringHash("DEH_ACTOR_188");
constexpr uint32_t kDehActor189           = epi::KnownStringHash("DEH_ACTOR_189");
constexpr uint32_t kDehActor190           = epi::KnownStringHash("DEH_ACTOR_190");
constexpr uint32_t kDehActor191           = epi::KnownStringHash("DEH_ACTOR_191");
constexpr uint32_t kDehActor192           = epi::KnownStringHash("DEH_ACTOR_192");
constexpr uint32_t kDehActor193           = epi::KnownStringHash("DEH_ACTOR_193");
constexpr uint32_t kDehActor194           = epi::KnownStringHash("DEH_ACTOR_194");
constexpr uint32_t kDehActor195           = epi::KnownStringHash("DEH_ACTOR_195");
constexpr uint32_t kDehActor196           = epi::KnownStringHash("DEH_ACTOR_196");
constexpr uint32_t kDehActor197           = epi::KnownStringHash("DEH_ACTOR_197");
constexpr uint32_t kDehActor198           = epi::KnownStringHash("DEH_ACTOR_198");
constexpr uint32_t kDehActor199           = epi::KnownStringHash("DEH_ACTOR_199");
constexpr uint32_t kDehActor200           = epi::KnownStringHash("DEH_ACTOR_200");
constexpr uint32_t kDehActor201           = epi::KnownStringHash("DEH_ACTOR_201");
constexpr uint32_t kDehActor202           = epi::KnownStringHash("DEH_ACTOR_202");
constexpr uint32_t kDehActor203           = epi::KnownStringHash("DEH_ACTOR_203");
constexpr uint32_t kDehActor204           = epi::KnownStringHash("DEH_ACTOR_204");
constexpr uint32_t kDehActor205           = epi::KnownStringHash("DEH_ACTOR_205");
constexpr uint32_t kDehActor206           = epi::KnownStringHash("DEH_ACTOR_206");
constexpr uint32_t kDehActor207           = epi::KnownStringHash("DEH_ACTOR_207");
constexpr uint32_t kDehActor208           = epi::KnownStringHash("DEH_ACTOR_208");
constexpr uint32_t kDehActor209           = epi::KnownStringHash("DEH_ACTOR_209");
constexpr uint32_t kDehActor210           = epi::KnownStringHash("DEH_ACTOR_210");
constexpr uint32_t kDehActor211           = epi::KnownStringHash("DEH_ACTOR_211");
constexpr uint32_t kDehActor212           = epi::KnownStringHash("DEH_ACTOR_212");
constexpr uint32_t kDehActor213           = epi::KnownStringHash("DEH_ACTOR_213");
constexpr uint32_t kDehActor214           = epi::KnownStringHash("DEH_ACTOR_214");
constexpr uint32_t kDehActor215           = epi::KnownStringHash("DEH_ACTOR_215");
constexpr uint32_t kDehActor216           = epi::KnownStringHash("DEH_ACTOR_216");
constexpr uint32_t kDehActor217           = epi::KnownStringHash("DEH_ACTOR_217");
constexpr uint32_t kDehActor218           = epi::KnownStringHash("DEH_ACTOR_218");
constexpr uint32_t kDehActor219           = epi::KnownStringHash("DEH_ACTOR_219");
constexpr uint32_t kDehActor220           = epi::KnownStringHash("DEH_ACTOR_220");
constexpr uint32_t kDehActor221           = epi::KnownStringHash("DEH_ACTOR_221");
constexpr uint32_t kDehActor222           = epi::KnownStringHash("DEH_ACTOR_222");
constexpr uint32_t kDehActor223           = epi::KnownStringHash("DEH_ACTOR_223");
constexpr uint32_t kDehActor224           = epi::KnownStringHash("DEH_ACTOR_224");
constexpr uint32_t kDehActor225           = epi::KnownStringHash("DEH_ACTOR_225");
constexpr uint32_t kDehActor226           = epi::KnownStringHash("DEH_ACTOR_226");
constexpr uint32_t kDehActor227           = epi::KnownStringHash("DEH_ACTOR_227");
constexpr uint32_t kDehActor228           = epi::KnownStringHash("DEH_ACTOR_228");
constexpr uint32_t kDehActor229           = epi::KnownStringHash("DEH_ACTOR_229");
constexpr uint32_t kDehActor230           = epi::KnownStringHash("DEH_ACTOR_230");
constexpr uint32_t kDehActor231           = epi::KnownStringHash("DEH_ACTOR_231");
constexpr uint32_t kDehActor232           = epi::KnownStringHash("DEH_ACTOR_232");
constexpr uint32_t kDehActor233           = epi::KnownStringHash("DEH_ACTOR_233");
constexpr uint32_t kDehActor234           = epi::KnownStringHash("DEH_ACTOR_234");
constexpr uint32_t kDehActor235           = epi::KnownStringHash("DEH_ACTOR_235");
constexpr uint32_t kDehActor236           = epi::KnownStringHash("DEH_ACTOR_236");
constexpr uint32_t kDehActor237           = epi::KnownStringHash("DEH_ACTOR_237");
constexpr uint32_t kDehActor238           = epi::KnownStringHash("DEH_ACTOR_238");
constexpr uint32_t kDehActor239           = epi::KnownStringHash("DEH_ACTOR_239");
constexpr uint32_t kDehActor240           = epi::KnownStringHash("DEH_ACTOR_240");
constexpr uint32_t kDehActor241           = epi::KnownStringHash("DEH_ACTOR_241");
constexpr uint32_t kDehActor242           = epi::KnownStringHash("DEH_ACTOR_242");
constexpr uint32_t kDehActor243           = epi::KnownStringHash("DEH_ACTOR_243");
constexpr uint32_t kDehActor244           = epi::KnownStringHash("DEH_ACTOR_244");
constexpr uint32_t kDehActor245           = epi::KnownStringHash("DEH_ACTOR_245");
constexpr uint32_t kDehActor246           = epi::KnownStringHash("DEH_ACTOR_246");
constexpr uint32_t kDehActor247           = epi::KnownStringHash("DEH_ACTOR_247");
constexpr uint32_t kDehActor248           = epi::KnownStringHash("DEH_ACTOR_248");
constexpr uint32_t kDehActor249           = epi::KnownStringHash("DEH_ACTOR_249");

} // namespace umapinfo

static std::unordered_map<uint32_t, std::pair<int16_t, int16_t>> ActorNames = {
    {umapinfo::kDoomPlayer, {1, -1}},
    {umapinfo::kZombieMan, {2, 3004}},
    {umapinfo::kShotgunGuy, {3, 9}},
    {umapinfo::kArchvile, {4, 64}},
    {umapinfo::kArchvileFire, {5, -1}},
    {umapinfo::kRevenant, {6, 66}},
    {umapinfo::kRevenantTracer, {7, -1}},
    {umapinfo::kRevenantTracerSmoke, {8, -1}},
    {umapinfo::kFatso, {9, 67}},
    {umapinfo::kFatShot, {10, -1}},
    {umapinfo::kChaingunGuy, {11, 65}},
    {umapinfo::kDoomImp, {12, 3001}},
    {umapinfo::kDemon, {13, 3002}},
    {umapinfo::kSpectre, {14, 58}},
    {umapinfo::kCacodemon, {15, 3005}},
    {umapinfo::kBaronOfHell, {16, 3003}},
    {umapinfo::kBaronBall, {17, -1}},
    {umapinfo::kHellKnight, {18, 69}},
    {umapinfo::kLostSoul, {19, 3006}},
    {umapinfo::kSpiderMastermind, {20, 7}},
    {umapinfo::kArachnotron, {21, 68}},
    {umapinfo::kCyberdemon, {22, 16}},
    {umapinfo::kPainElemental, {23, 71}},
    {umapinfo::kWolfensteinSS, {24, 84}},
    {umapinfo::kCommanderKeen, {25, 72}},
    {umapinfo::kBossBrain, {26, 88}},
    {umapinfo::kBossEye, {27, 89}},
    {umapinfo::kBossTarget, {28, 87}},
    {umapinfo::kSpawnShot, {29, -1}},
    {umapinfo::kSpawnFire, {30, -1}},
    {umapinfo::kExplosiveBarrel, {31, 2035}},
    {umapinfo::kDoomImpBall, {32, -1}},
    {umapinfo::kCacodemonBall, {33, -1}},
    {umapinfo::kRocket, {34, -1}},
    {umapinfo::kPlasmaBall, {35, -1}},
    {umapinfo::kBFGBall, {36, -1}},
    {umapinfo::kArachnotronPlasma, {37, -1}},
    {umapinfo::kBulletPuff, {38, -1}},
    {umapinfo::kBlood, {39, -1}},
    {umapinfo::kTeleportFog, {40, -1}},
    {umapinfo::kItemFog, {41, -1}},
    {umapinfo::kTeleportDest, {42, 14}},
    {umapinfo::kBFGExtra, {43, -1}},
    {umapinfo::kGreenArmor, {44, 2018}},
    {umapinfo::kBlueArmor, {45, 2019}},
    {umapinfo::kHealthBonus, {46, 2014}},
    {umapinfo::kArmorBonus, {47, 2015}},
    {umapinfo::kBlueCard, {48, 5}},
    {umapinfo::kRedCard, {49, 13}},
    {umapinfo::kYellowCard, {50, 6}},
    {umapinfo::kYellowSkull, {51, 39}},
    {umapinfo::kRedSkull, {52, 38}},
    {umapinfo::kBlueSkull, {53, 40}},
    {umapinfo::kStimpack, {54, 2011}},
    {umapinfo::kMedikit, {55, 2012}},
    {umapinfo::kSoulsphere, {56, 2013}},
    {umapinfo::kInvulnerabilitySphere, {57, 2022}},
    {umapinfo::kBerserk, {58, 2023}},
    {umapinfo::kBlurSphere, {59, 2024}},
    {umapinfo::kRadSuit, {60, 2025}},
    {umapinfo::kAllmap, {61, 2026}},
    {umapinfo::kInfrared, {62, 2045}},
    {umapinfo::kMegasphere, {63, 83}},
    {umapinfo::kClip, {64, 2007}},
    {umapinfo::kClipBox, {65, 2048}},
    {umapinfo::kRocketAmmo, {66, 2010}},
    {umapinfo::kRocketBox, {67, 2046}},
    {umapinfo::kCell, {68, 2047}},
    {umapinfo::kCellPack, {69, 17}},
    {umapinfo::kShell, {70, 2008}},
    {umapinfo::kShellBox, {71, 2049}},
    {umapinfo::kBackpack, {72, 8}},
    {umapinfo::kBFG9000, {73, 2006}},
    {umapinfo::kChaingun, {74, 2002}},
    {umapinfo::kChainsaw, {75, 2005}},
    {umapinfo::kRocketLauncher, {76, 2003}},
    {umapinfo::kPlasmaRifle, {77, 2004}},
    {umapinfo::kShotgun, {78, 2001}},
    {umapinfo::kSuperShotgun, {79, 82}},
    {umapinfo::kTechLamp, {80, 85}},
    {umapinfo::kTechLamp2, {81, 86}},
    {umapinfo::kColumn, {82, 2028}},
    {umapinfo::kTallGreenColumn, {83, 30}},
    {umapinfo::kShortGreenColumn, {84, 31}},
    {umapinfo::kTallRedColumn, {85, 32}},
    {umapinfo::kShortRedColumn, {86, 33}},
    {umapinfo::kSkullColumn, {87, 37}},
    {umapinfo::kHeartColumn, {88, 36}},
    {umapinfo::kEvilEye, {89, 41}},
    {umapinfo::kFloatingSkull, {90, 42}},
    {umapinfo::kTorchTree, {91, 43}},
    {umapinfo::kBlueTorch, {92, 44}},
    {umapinfo::kGreenTorch, {93, 45}},
    {umapinfo::kRedTorch, {94, 46}},
    {umapinfo::kShortBlueTorch, {95, 55}},
    {umapinfo::kShortGreenTorch, {96, 56}},
    {umapinfo::kShortRedTorch, {97, 57}},
    {umapinfo::kStalagtite, {98, 47}},
    {umapinfo::kTechPillar, {99, 48}},
    {umapinfo::kCandleStick, {100, 34}},
    {umapinfo::kCandelabra, {101, 35}},
    {umapinfo::kBloodyTwitch, {102, 49}},
    {umapinfo::kMeat2, {103, 50}},
    {umapinfo::kMeat3, {104, 51}},
    {umapinfo::kMeat4, {105, 52}},
    {umapinfo::kMeat5, {106, 53}},
    {umapinfo::kNonsolidMeat2, {107, 59}},
    {umapinfo::kNonsolidMeat4, {108, 60}},
    {umapinfo::kNonsolidMeat3, {109, 61}},
    {umapinfo::kNonsolidMeat5, {110, 62}},
    {umapinfo::kNonsolidTwitch, {111, 63}},
    {umapinfo::kDeadCacodemon, {112, 22}},
    {umapinfo::kDeadMarine, {113, 15}},
    {umapinfo::kDeadZombieMan, {114, 18}},
    {umapinfo::kDeadDemon, {115, 21}},
    {umapinfo::kDeadLostSoul, {116, 23}},
    {umapinfo::kDeadDoomImp, {117, 20}},
    {umapinfo::kDeadShotgunGuy, {118, 19}},
    {umapinfo::kGibbedMarine, {119, 10}},
    {umapinfo::kGibbedMarineExtra, {120, 12}},
    {umapinfo::kHeadsOnAStick, {121, 28}},
    {umapinfo::kGibs, {122, 24}},
    {umapinfo::kHeadOnAStick, {123, 27}},
    {umapinfo::kHeadCandles, {124, 29}},
    {umapinfo::kDeadStick, {125, 25}},
    {umapinfo::kLiveStick, {126, 26}},
    {umapinfo::kBigTree, {127, 54}},
    {umapinfo::kBurningBarrel, {128, 70}},
    {umapinfo::kHangNoGuts, {129, 73}},
    {umapinfo::kHangBNoBrain, {130, 74}},
    {umapinfo::kHangTLookingDown, {131, 75}},
    {umapinfo::kHangTSkull, {132, 76}},
    {umapinfo::kHangTLookingUp, {133, 77}},
    {umapinfo::kHangTNoBrain, {134, 78}},
    {umapinfo::kColonGibs, {135, 79}},
    {umapinfo::kSmallBloodPool, {136, 80}},
    {umapinfo::kBrainStem, {137, 81}},
    // Boom/MBF additions
    {umapinfo::kPointPusher, {138, 5001}},
    {umapinfo::kPointPuller, {139, 5002}},
    {umapinfo::kMBFHelperDog, {140, 888}},
    {umapinfo::kPlasmaBall1, {141, -1}},
    {umapinfo::kPlasmaBall2, {142, -1}},
    {umapinfo::kEvilSceptre, {143, -1}},
    {umapinfo::kUnholyBible, {144, -1}},
    {umapinfo::kMusicChanger, {145, -1}},
    {umapinfo::kDehActor145, {145, -1}},
    {umapinfo::kDehActor146, {146, -1}},
    {umapinfo::kDehActor147, {147, -1}},
    {umapinfo::kDehActor148, {148, -1}},
    {umapinfo::kDehActor149, {149, -1}},
    // DEHEXTRA Actors start here
    {umapinfo::kDehActor150, {151, -1}}, // MT_EXTRA0
    {umapinfo::kDehActor151, {152, -1}}, // MT_EXTRA1
    {umapinfo::kDehActor152, {153, -1}}, // MT_EXTRA2
    {umapinfo::kDehActor153, {154, -1}}, // MT_EXTRA3
    {umapinfo::kDehActor154, {155, -1}}, // MT_EXTRA4
    {umapinfo::kDehActor155, {156, -1}}, // MT_EXTRA5
    {umapinfo::kDehActor156, {157, -1}}, // MT_EXTRA6
    {umapinfo::kDehActor157, {158, -1}}, // MT_EXTRA7
    {umapinfo::kDehActor158, {159, -1}}, // MT_EXTRA8
    {umapinfo::kDehActor159, {160, -1}}, // MT_EXTRA9
    {umapinfo::kDehActor160, {161, -1}}, // MT_EXTRA10
    {umapinfo::kDehActor161, {162, -1}}, // MT_EXTRA11
    {umapinfo::kDehActor162, {163, -1}}, // MT_EXTRA12
    {umapinfo::kDehActor163, {164, -1}}, // MT_EXTRA13
    {umapinfo::kDehActor164, {165, -1}}, // MT_EXTRA14
    {umapinfo::kDehActor165, {166, -1}}, // MT_EXTRA15
    {umapinfo::kDehActor166, {167, -1}}, // MT_EXTRA16
    {umapinfo::kDehActor167, {168, -1}}, // MT_EXTRA17
    {umapinfo::kDehActor168, {169, -1}}, // MT_EXTRA18
    {umapinfo::kDehActor169, {170, -1}}, // MT_EXTRA19
    {umapinfo::kDehActor170, {171, -1}}, // MT_EXTRA20
    {umapinfo::kDehActor171, {172, -1}}, // MT_EXTRA21
    {umapinfo::kDehActor172, {173, -1}}, // MT_EXTRA22
    {umapinfo::kDehActor173, {174, -1}}, // MT_EXTRA23
    {umapinfo::kDehActor174, {175, -1}}, // MT_EXTRA24
    {umapinfo::kDehActor175, {176, -1}}, // MT_EXTRA25
    {umapinfo::kDehActor176, {177, -1}}, // MT_EXTRA26
    {umapinfo::kDehActor177, {178, -1}}, // MT_EXTRA27
    {umapinfo::kDehActor178, {179, -1}}, // MT_EXTRA28
    {umapinfo::kDehActor179, {180, -1}}, // MT_EXTRA29
    {umapinfo::kDehActor180, {181, -1}}, // MT_EXTRA30
    {umapinfo::kDehActor181, {182, -1}}, // MT_EXTRA31
    {umapinfo::kDehActor182, {183, -1}}, // MT_EXTRA32
    {umapinfo::kDehActor183, {184, -1}}, // MT_EXTRA33
    {umapinfo::kDehActor184, {185, -1}}, // MT_EXTRA34
    {umapinfo::kDehActor185, {186, -1}}, // MT_EXTRA35
    {umapinfo::kDehActor186, {187, -1}}, // MT_EXTRA36
    {umapinfo::kDehActor187, {188, -1}}, // MT_EXTRA37
    {umapinfo::kDehActor188, {189, -1}}, // MT_EXTRA38
    {umapinfo::kDehActor189, {190, -1}}, // MT_EXTRA39
    {umapinfo::kDehActor190, {191, -1}}, // MT_EXTRA40
    {umapinfo::kDehActor191, {192, -1}}, // MT_EXTRA41
    {umapinfo::kDehActor192, {193, -1}}, // MT_EXTRA42
    {umapinfo::kDehActor193, {194, -1}}, // MT_EXTRA43
    {umapinfo::kDehActor194, {195, -1}}, // MT_EXTRA44
    {umapinfo::kDehActor195, {196, -1}}, // MT_EXTRA45
    {umapinfo::kDehActor196, {197, -1}}, // MT_EXTRA46
    {umapinfo::kDehActor197, {198, -1}}, // MT_EXTRA47
    {umapinfo::kDehActor198, {199, -1}}, // MT_EXTRA48
    {umapinfo::kDehActor199, {200, -1}}, // MT_EXTRA49
    {umapinfo::kDehActor200, {201, -1}}, // MT_EXTRA50
    {umapinfo::kDehActor201, {202, -1}}, // MT_EXTRA51
    {umapinfo::kDehActor202, {203, -1}}, // MT_EXTRA52
    {umapinfo::kDehActor203, {204, -1}}, // MT_EXTRA53
    {umapinfo::kDehActor204, {205, -1}}, // MT_EXTRA54
    {umapinfo::kDehActor205, {206, -1}}, // MT_EXTRA55
    {umapinfo::kDehActor206, {207, -1}}, // MT_EXTRA56
    {umapinfo::kDehActor207, {208, -1}}, // MT_EXTRA57
    {umapinfo::kDehActor208, {209, -1}}, // MT_EXTRA58
    {umapinfo::kDehActor209, {210, -1}}, // MT_EXTRA59
    {umapinfo::kDehActor210, {211, -1}}, // MT_EXTRA60
    {umapinfo::kDehActor211, {212, -1}}, // MT_EXTRA61
    {umapinfo::kDehActor212, {213, -1}}, // MT_EXTRA62
    {umapinfo::kDehActor213, {214, -1}}, // MT_EXTRA63
    {umapinfo::kDehActor214, {215, -1}}, // MT_EXTRA64
    {umapinfo::kDehActor215, {216, -1}}, // MT_EXTRA65
    {umapinfo::kDehActor216, {217, -1}}, // MT_EXTRA66
    {umapinfo::kDehActor217, {218, -1}}, // MT_EXTRA67
    {umapinfo::kDehActor218, {219, -1}}, // MT_EXTRA68
    {umapinfo::kDehActor219, {220, -1}}, // MT_EXTRA69
    {umapinfo::kDehActor220, {221, -1}}, // MT_EXTRA70
    {umapinfo::kDehActor221, {222, -1}}, // MT_EXTRA71
    {umapinfo::kDehActor222, {223, -1}}, // MT_EXTRA72
    {umapinfo::kDehActor223, {224, -1}}, // MT_EXTRA73
    {umapinfo::kDehActor224, {225, -1}}, // MT_EXTRA74
    {umapinfo::kDehActor225, {226, -1}}, // MT_EXTRA75
    {umapinfo::kDehActor226, {227, -1}}, // MT_EXTRA76
    {umapinfo::kDehActor227, {228, -1}}, // MT_EXTRA77
    {umapinfo::kDehActor228, {229, -1}}, // MT_EXTRA78
    {umapinfo::kDehActor229, {230, -1}}, // MT_EXTRA79
    {umapinfo::kDehActor230, {231, -1}}, // MT_EXTRA80
    {umapinfo::kDehActor231, {232, -1}}, // MT_EXTRA81
    {umapinfo::kDehActor232, {233, -1}}, // MT_EXTRA82
    {umapinfo::kDehActor233, {234, -1}}, // MT_EXTRA83
    {umapinfo::kDehActor234, {235, -1}}, // MT_EXTRA84
    {umapinfo::kDehActor235, {236, -1}}, // MT_EXTRA85
    {umapinfo::kDehActor236, {237, -1}}, // MT_EXTRA86
    {umapinfo::kDehActor237, {238, -1}}, // MT_EXTRA87
    {umapinfo::kDehActor238, {239, -1}}, // MT_EXTRA88
    {umapinfo::kDehActor239, {240, -1}}, // MT_EXTRA89
    {umapinfo::kDehActor240, {241, -1}}, // MT_EXTRA90
    {umapinfo::kDehActor241, {242, -1}}, // MT_EXTRA91
    {umapinfo::kDehActor242, {243, -1}}, // MT_EXTRA92
    {umapinfo::kDehActor243, {244, -1}}, // MT_EXTRA93
    {umapinfo::kDehActor244, {245, -1}}, // MT_EXTRA94
    {umapinfo::kDehActor245, {246, -1}}, // MT_EXTRA95
    {umapinfo::kDehActor246, {247, -1}}, // MT_EXTRA96
    {umapinfo::kDehActor247, {248, -1}}, // MT_EXTRA97
    {umapinfo::kDehActor248, {249, -1}}, // MT_EXTRA98
    {umapinfo::kDehActor249, {250, -1}}  // MT_EXTRA99
};

static void FreeMap(MapEntry *mape)
{
    if (mape->mapname)
        free(mape->mapname);
    if (mape->levelname)
        free(mape->levelname);
    if (mape->label)
        free(mape->label);
    if (mape->intertext)
        free(mape->intertext);
    if (mape->intertextsecret)
        free(mape->intertextsecret);
    if (mape->bossactions)
        free(mape->bossactions);
    if (mape->authorname)
        free(mape->authorname);
    mape->mapname = nullptr;
}

void FreeMapList()
{
    unsigned i;

    for (i = 0; i < Maps.mapcount; i++)
    {
        FreeMap(&Maps.maps[i]);
    }
    free(Maps.maps);
    Maps.maps     = nullptr;
    Maps.mapcount = 0;
}

// -----------------------------------------------
//
// Parses a complete UMAPINFO entry
//
// -----------------------------------------------

static void ParseUMAPINFOEntry(epi::Scanner &lex, MapEntry *val)
{
    for (;;)
    {
        if (lex.CheckToken('}'))
            break;

        std::string key;
        std::string value;

        if (!lex.GetNextToken())
            FatalError("Malformed UMAPINFO lump: unclosed block\n");

        if (lex.state_.token != epi::Scanner::kIdentifier)
            FatalError("Malformed UMAPINFO lump: missing key\n");

        key = lex.state_.string;

        if (!lex.CheckToken('='))
            FatalError("Malformed UMAPINFO lump: missing '='\n");

        if (!lex.GetNextToken() || lex.state_.token == '}')
            FatalError("Malformed UMAPINFO lump: missing value\n");

        epi::StringHash key_hash(key);

        switch (key_hash.Value())
        {
        case umapinfo::kLevelName: {
            if (val->levelname)
                free(val->levelname);
            value          = lex.state_.string;
            val->levelname = (char *)calloc(value.size() + 1, sizeof(char));
            epi::CStringCopyMax(val->levelname, value.c_str(), value.size());
        }
        break;
        case umapinfo::kLabel: {
            if (epi::StringCaseCompareASCII(value, "clear") == 0)
            {
                if (val->label)
                    free(val->label);
                val->label    = (char *)calloc(2, sizeof(char));
                val->label[0] = '-';
            }
            else
            {
                if (val->label)
                    free(val->label);
                value      = lex.state_.string;
                val->label = (char *)calloc(value.size() + 1, sizeof(char));
                epi::CStringCopyMax(val->label, value.c_str(), value.size());
            }
        }
        break;
        case umapinfo::kNext: {
            EPI_CLEAR_MEMORY(val->next_map, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Mapname for \"next\" over 8 characters!\n");
            epi::CStringCopyMax(val->next_map, value.data(), 8);
        }
        break;
        case umapinfo::kNextSecret: {
            EPI_CLEAR_MEMORY(val->nextsecret, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Mapname for \"nextsecret\" over 8 "
                           "characters!\n");
            epi::CStringCopyMax(val->nextsecret, value.data(), 8);
        }
        break;
        case umapinfo::kLevelPic: {
            EPI_CLEAR_MEMORY(val->levelpic, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"levelpic\" over 8 "
                           "characters!\n");
            epi::CStringCopyMax(val->levelpic, value.data(), 8);
        }
        break;
        case umapinfo::kSkyTexture: {
            EPI_CLEAR_MEMORY(val->skytexture, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"skytexture\" over 8 "
                           "characters!\n");
            epi::CStringCopyMax(val->skytexture, value.data(), 8);
        }
        break;
        case umapinfo::kMusic: {
            EPI_CLEAR_MEMORY(val->music, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"music\" over 8 characters!\n");
            epi::CStringCopyMax(val->music, value.data(), 8);
        }
        break;
        case umapinfo::kEndPic: {
            EPI_CLEAR_MEMORY(val->endpic, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"endpic\" over 8 characters!\n");
            epi::CStringCopyMax(val->endpic, value.data(), 8);
        }
        break;
        case umapinfo::kEndCast:
            val->docast = lex.state_.boolean;
            break;
        case umapinfo::kEndBunny:
            val->dobunny = lex.state_.boolean;
            break;
        case umapinfo::kEndGame:
            val->endgame = lex.state_.boolean;
            break;
        case umapinfo::kExitPic: {
            EPI_CLEAR_MEMORY(val->exitpic, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"exitpic\" over 8 characters!\n");
            epi::CStringCopyMax(val->exitpic, value.data(), 8);
        }
        break;
        case umapinfo::kEnterPic: {
            EPI_CLEAR_MEMORY(val->enterpic, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"enterpic\" over 8 "
                           "characters!\n");
            epi::CStringCopyMax(val->enterpic, value.data(), 8);
        }
        break;
        case umapinfo::kNoIntermission:
            val->nointermission = lex.state_.boolean;
            break;
        case umapinfo::kParTime:
            val->partime = 35 * lex.state_.number;
            break;
        case umapinfo::kIntertext: {
            value = lex.state_.string;
            while (lex.CheckToken(','))
            {
                value.append("\n");
                lex.GetNextToken();
                value.append(lex.state_.string);
            }
            if (val->intertext)
                free(val->intertext);
            val->intertext = (char *)calloc(value.size() + 1, sizeof(char));
            epi::CStringCopyMax(val->intertext, value.c_str(), value.size());
        }
        break;
        case umapinfo::kIntertextSecret: {
            value = lex.state_.string;
            while (lex.CheckToken(','))
            {
                value.append("\n");
                lex.GetNextToken();
                value.append(lex.state_.string);
            }
            if (val->intertextsecret)
                free(val->intertextsecret);
            val->intertextsecret = (char *)calloc(value.size() + 1, sizeof(char));
            epi::CStringCopyMax(val->intertextsecret, value.c_str(), value.size());
        }
        break;
        case umapinfo::kInterbackdrop: {
            EPI_CLEAR_MEMORY(val->interbackdrop, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"interbackdrop\" over 8 "
                           "characters!\n");
            epi::CStringCopyMax(val->interbackdrop, value.data(), 8);
        }
        break;
        case umapinfo::kIntermusic: {
            EPI_CLEAR_MEMORY(val->intermusic, char, 9);
            value = lex.state_.string;
            if (value.size() > 8)
                FatalError("UMAPINFO: Entry for \"intermusic\" over 8 "
                           "characters!\n");
            epi::CStringCopyMax(val->intermusic, value.data(), 8);
        }
        break;
        case umapinfo::kEpisode: {
            value = lex.state_.string;
            if (epi::StringCaseCompareASCII(value, "clear") == 0)
            {
                // This should leave the initial [EDGE] episode and nothing
                // else Since 'clear' is supposed to come before any custom
                // definitions this should not clear out any
                // UMAPINFO-defined episodes
                for (std::vector<GameDefinition *>::iterator iter = gamedefs.begin() + 1; iter != gamedefs.end();)
                {
                    GameDefinition *game = *iter;
                    if (game->firstmap_.empty() && epi::StringCaseCompareASCII(game->name_, "UMAPINFO_TEMPLATE") != 0)
                    {
                        delete game;
                        game = nullptr;
                        iter = gamedefs.erase(iter);
                    }
                    else
                        ++iter;
                }
            }
            else
            {
                GameDefinition *new_epi = nullptr;
                // Check for episode to replace
                for (GameDefinition *game : gamedefs)
                {
                    if (epi::StringCaseCompareASCII(game->firstmap_, val->mapname) == 0 &&
                        epi::StringCaseCompareASCII(game->name_, "UMAPINFO_TEMPLATE") != 0)
                    {
                        new_epi = game;
                        break;
                    }
                }
                if (!new_epi)
                {
                    // Create a new episode from game-specific UMAPINFO
                    // template data
                    GameDefinition *um_template = nullptr;
                    for (GameDefinition *game : gamedefs)
                    {
                        if (epi::StringCaseCompareASCII(game->name_, "UMAPINFO_TEMPLATE") == 0)
                        {
                            um_template = game;
                            break;
                        }
                    }
                    if (!um_template)
                        FatalError("UMAPINFO: No custom episode template exists "
                                   "for this IWAD! Check DDFGAME!\n");
                    new_epi = new GameDefinition;
                    new_epi->CopyDetail(*um_template);
                    new_epi->firstmap_ = val->mapname;
                    gamedefs.push_back(new_epi);
                }
                char        lumpname[9] = {0};
                std::string alttext;
                std::string epikey; // Do we use this?
                if (value.size() > 8)
                    FatalError("UMAPINFO: Entry for \"enterpic\" over 8 "
                               "characters!\n");
                epi::CStringCopyMax(lumpname, value.data(), 8);
                if (lex.CheckToken(','))
                {
                    lex.GetNextToken();
                    alttext = lex.state_.string;
                    if (lex.CheckToken(','))
                    {
                        lex.GetNextToken();
                        epikey = lex.state_.string;
                    }
                }
                new_epi->namegraphic_ = lumpname;
                new_epi->description_ = alttext;
                new_epi->name_        = epi::StringFormat("UMAPINFO_%s\n", val->mapname); // Internal
            }
        }
        break;
        case umapinfo::kBossAction: {
            int special = 0;
            int tag     = 0;
            value       = lex.state_.string;
            if (epi::StringCaseCompareASCII(value, "clear") == 0)
            {
                special = tag = -1;
                if (val->bossactions)
                    free(val->bossactions);
                val->bossactions    = nullptr;
                val->numbossactions = -1;
            }
            else
            {
                int             actor_num = -1;
                epi::StringHash actor_check(value);
                if (!ActorNames.count(actor_check.Value()))
                    FatalError("UMAPINFO: Unknown thing type %s\n", value.c_str());
                else
                {
                    std::pair<int16_t, int16_t> nums = ActorNames[actor_check.Value()];
                    if (nums.second != -1) // DoomEd number exists already
                        actor_num = nums.second;
                    else                   // See if modified by Dehacked, else skip
                    {
                        for (const MapObjectDefinition *mob : mobjtypes)
                        {
                            if (mob->deh_thing_id_ == nums.first)
                            {
                                actor_num = mob->number_;
                                break;
                            }
                        }
                    }
                }
                if (actor_num == -1)
                    lex.SkipLine();
                else
                {
                    if (!lex.CheckToken(','))
                        FatalError("UMAPINFO: \"bossaction\" key missing line "
                                   "special!\n");
                    lex.GetNextToken();
                    special = lex.state_.number;
                    if (!lex.CheckToken(','))
                        FatalError("UMAPINFO: \"bossaction\" key missing tag!\n");
                    lex.GetNextToken();
                    tag = lex.state_.number;
                    if (tag != 0 || special == 11 || special == 51 || special == 52 || special == 124)
                    {
                        if (val->numbossactions == -1)
                            val->numbossactions = 1;
                        else
                            val->numbossactions++;
                        val->bossactions = (struct BossAction *)realloc(val->bossactions, sizeof(struct BossAction) *
                                                                                              val->numbossactions);
                        val->bossactions[val->numbossactions - 1].type    = actor_num;
                        val->bossactions[val->numbossactions - 1].special = special;
                        val->bossactions[val->numbossactions - 1].tag     = tag;
                    }
                }
            }
        }
        break;
        case umapinfo::kAuthor: {
            if (val->authorname)
                free(val->authorname);
            value           = lex.state_.string;
            val->authorname = (char *)calloc(value.size() + 1, sizeof(char));
            epi::CStringCopyMax(val->authorname, value.c_str(), value.size());
        }
        break;
        default:
            break;
        }
    }
    // Some fallback handling
    if (!val->nextsecret[0])
    {
        if (val->next_map[0])
            epi::CStringCopyMax(val->nextsecret, val->next_map, 8);
    }
    if (!val->enterpic[0])
    {
        for (size_t i = 0; i < Maps.mapcount; i++)
        {
            if (!strcmp(val->mapname, Maps.maps[i].next_map))
            {
                if (Maps.maps[i].exitpic[0])
                    epi::CStringCopyMax(val->enterpic, Maps.maps[i].exitpic, 8);
                break;
            }
        }
    }
}

// -----------------------------------------------
//
// Parses a complete UMAPINFO lump
//
// -----------------------------------------------

void ParseUMAPINFO(const std::string &buffer)
{
    epi::Scanner lex(buffer);

    while (lex.TokensLeft())
    {
        if (!lex.GetNextToken())
            break;

        if (lex.state_.token != epi::Scanner::kIdentifier || epi::StringCaseCompareASCII(lex.state_.string, "MAP") != 0)
            FatalError("Malformed UMAPINFO lump.\n");

        lex.GetNextToken();

        if (lex.state_.token != epi::Scanner::kIdentifier)
            FatalError("UMAPINFO: No mapname for map entry!\n");

        unsigned int i = 0;
        MapEntry     parsed;
        EPI_CLEAR_MEMORY(&parsed, MapEntry, 1);
        parsed.mapname = (char *)calloc(lex.state_.string.size() + 1, sizeof(char));
        epi::CStringCopyMax(parsed.mapname, lex.state_.string.data(), lex.state_.string.size());

        if (!lex.CheckToken('{'))
            FatalError("Malformed UMAPINFO lump: missing '{'\n");

        ParseUMAPINFOEntry(lex, &parsed);
        // Does this map entry already exist? If yes, replace it.
        for (i = 0; i < Maps.mapcount; i++)
        {
            if (epi::StringCaseCompareASCII(parsed.mapname, Maps.maps[i].mapname) == 0)
            {
                FreeMap(&Maps.maps[i]);
                Maps.maps[i] = parsed;
                break;
            }
        }
        // Not found so create a new one.
        if (i == Maps.mapcount)
        {
            Maps.mapcount++;
            Maps.maps                    = (MapEntry *)realloc(Maps.maps, sizeof(MapEntry) * Maps.mapcount);
            Maps.maps[Maps.mapcount - 1] = parsed;
        }
    }
}