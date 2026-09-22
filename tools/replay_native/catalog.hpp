// Adapter DAT mappings mirrored from BwapiBridge; checked by tests/test_replay_catalog.py.
#pragma once
protodd::UnitKind replayKind(const bwgame::UnitTypes type) noexcept {
    using enum bwgame::UnitTypes;
    using protodd::UnitKind;
    if (type == Protoss_Probe) return UnitKind::probe;
    if (type == Protoss_Nexus) return UnitKind::nexus;
    if (type == Protoss_Pylon) return UnitKind::pylon;
    if (type == Protoss_Assimilator) return UnitKind::assimilator;
    if (type == Protoss_Gateway) return UnitKind::gateway;
    if (type == Protoss_Forge) return UnitKind::forge;
    if (type == Protoss_Photon_Cannon) return UnitKind::photonCannon;
    if (type == Protoss_Cybernetics_Core) return UnitKind::cyberneticsCore;
    if (type == Protoss_Shield_Battery) return UnitKind::shieldBattery;
    if (type == Protoss_Robotics_Facility) return UnitKind::roboticsFacility;
    if (type == Protoss_Observatory) return UnitKind::observatory;
    if (type == Protoss_Robotics_Support_Bay) return UnitKind::roboticsSupportBay;
    if (type == Protoss_Stargate) return UnitKind::stargate;
    if (type == Protoss_Citadel_of_Adun) return UnitKind::citadelOfAdun;
    if (type == Protoss_Templar_Archives) return UnitKind::templarArchives;
    if (type == Protoss_Fleet_Beacon) return UnitKind::fleetBeacon;
    if (type == Protoss_Arbiter_Tribunal) return UnitKind::arbiterTribunal;
    if (type == Protoss_Zealot) return UnitKind::zealot;
    if (type == Protoss_Dragoon) return UnitKind::dragoon;
    if (type == Protoss_High_Templar) return UnitKind::highTemplar;
    if (type == Protoss_Dark_Templar) return UnitKind::darkTemplar;
    if (type == Protoss_Archon) return UnitKind::archon;
    if (type == Protoss_Dark_Archon) return UnitKind::darkArchon;
    if (type == Protoss_Reaver) return UnitKind::reaver;
    if (type == Protoss_Observer) return UnitKind::observer;
    if (type == Protoss_Shuttle) return UnitKind::shuttle;
    if (type == Protoss_Scout) return UnitKind::scout;
    if (type == Protoss_Corsair) return UnitKind::corsair;
    if (type == Protoss_Carrier) return UnitKind::carrier;
    if (type == Protoss_Arbiter) return UnitKind::arbiter;
    if (type == Terran_SCV) return UnitKind::scv;
    if (type == Terran_Command_Center) return UnitKind::commandCenter;
    if (type == Terran_Supply_Depot) return UnitKind::supplyProvider;
    if (type == Terran_Refinery) return UnitKind::refinery;
    if (type == Terran_Barracks) return UnitKind::barracks;
    if (type == Terran_Factory) return UnitKind::factory;
    if (type == Terran_Starport) return UnitKind::starport;
    if (type == Terran_Bunker) return UnitKind::bunker;
    if (type == Terran_Missile_Turret) return UnitKind::missileTurret;
    if (type == Terran_Marine) return UnitKind::marine;
    if (type == Terran_Medic) return UnitKind::medic;
    if (type == Terran_Firebat) return UnitKind::firebat;
    if (type == Terran_Vulture) return UnitKind::vulture;
    if (type == Terran_Siege_Tank_Tank_Mode || type == Terran_Siege_Tank_Siege_Mode)
        return UnitKind::siegeTank;
    if (type == Terran_Goliath) return UnitKind::goliath;
    if (type == Terran_Wraith) return UnitKind::wraith;
    if (type == Terran_Science_Vessel) return UnitKind::scienceVessel;
    if (type == Terran_Dropship) return UnitKind::dropship;
    if (type == Terran_Battlecruiser) return UnitKind::battlecruiser;
    if (type == Terran_Ghost) return UnitKind::ghost;
    if (type == Terran_Valkyrie) return UnitKind::valkyrie;
    if (type == Terran_Vulture_Spider_Mine) return UnitKind::spiderMine;
    if (type == Terran_Academy) return UnitKind::academy;
    if (type == Terran_Engineering_Bay) return UnitKind::engineeringBay;
    if (type == Terran_Armory) return UnitKind::armory;
    if (type == Terran_Machine_Shop) return UnitKind::machineShop;
    if (type == Terran_Control_Tower) return UnitKind::controlTower;
    if (type == Terran_Science_Facility) return UnitKind::scienceFacility;
    if (type == Terran_Covert_Ops) return UnitKind::covertOps;
    if (type == Terran_Physics_Lab) return UnitKind::physicsLab;
    if (type == Terran_Comsat_Station) return UnitKind::comsatStation;
    if (type == Terran_Nuclear_Silo) return UnitKind::nuclearSilo;
    if (type == Zerg_Drone) return UnitKind::drone;
    if (type == Zerg_Hatchery) return UnitKind::hatchery;
    if (type == Zerg_Infested_Command_Center) return UnitKind::commandCenter;
    if (type == Zerg_Extractor) return UnitKind::refinery;
    if (type == Zerg_Lair) return UnitKind::lair;
    if (type == Zerg_Hive) return UnitKind::hive;
    if (type == Zerg_Spawning_Pool) return UnitKind::spawningPool;
    if (type == Zerg_Hydralisk_Den) return UnitKind::hydraliskDen;
    if (type == Zerg_Spire) return UnitKind::spire;
    if (type == Zerg_Greater_Spire) return UnitKind::greaterSpire;
    if (type == Zerg_Sunken_Colony) return UnitKind::sunkenColony;
    if (type == Zerg_Spore_Colony) return UnitKind::sporeColony;
    if (type == Zerg_Zergling) return UnitKind::zergling;
    if (type == Zerg_Hydralisk) return UnitKind::hydralisk;
    if (type == Zerg_Lurker) return UnitKind::lurker;
    if (type == Zerg_Mutalisk) return UnitKind::mutalisk;
    if (type == Zerg_Scourge) return UnitKind::scourge;
    if (type == Zerg_Ultralisk) return UnitKind::ultralisk;
    if (type == Zerg_Defiler) return UnitKind::defiler;
    if (type == Zerg_Overlord) return UnitKind::overlord;
    if (type == Zerg_Queen) return UnitKind::queen;
    if (type == Zerg_Guardian) return UnitKind::guardian;
    if (type == Zerg_Devourer) return UnitKind::devourer;
    if (type == Zerg_Broodling) return UnitKind::broodling;
    if (type == Zerg_Infested_Terran) return UnitKind::infestedTerran;
    if (type == Zerg_Creep_Colony) return UnitKind::creepColony;
    if (type == Zerg_Evolution_Chamber) return UnitKind::evolutionChamber;
    if (type == Zerg_Queens_Nest) return UnitKind::queensNest;
    if (type == Zerg_Ultralisk_Cavern) return UnitKind::ultraliskCavern;
    if (type == Zerg_Defiler_Mound) return UnitKind::defilerMound;
    if (type == Zerg_Nydus_Canal) return UnitKind::nydusCanal;
    if (type == Zerg_Lurker_Egg) return UnitKind::lurkerEgg;
    if (type == Zerg_Cocoon) return UnitKind::cocoon;
    return UnitKind::unknown;
}

bwgame::TechTypes replayTech(const protodd::TechnologyKind kind) noexcept {
    using enum bwgame::TechTypes;
    using protodd::TechnologyKind;
    switch (kind) {
        case TechnologyKind::psionicStorm: return Psionic_Storm;
        case TechnologyKind::stasisField: return Stasis_Field;
        case TechnologyKind::recall: return Recall;
        default: return static_cast<bwgame::TechTypes>(-1);
    }
}

bwgame::UpgradeTypes replayUpgrade(const protodd::TechnologyKind kind) noexcept {
    using enum bwgame::UpgradeTypes;
    using protodd::TechnologyKind;
    switch (kind) {
        case TechnologyKind::singularityCharge: return Singularity_Charge;
        case TechnologyKind::legEnhancements: return Leg_Enhancements;
        case TechnologyKind::khaydarinAmulet: return Khaydarin_Amulet;
        case TechnologyKind::graviticDrive: return Gravitic_Drive;
        case TechnologyKind::graviticBoosters: return Gravitic_Boosters;
        case TechnologyKind::sensorArray: return Sensor_Array;
        case TechnologyKind::reaverCapacity: return Reaver_Capacity;
        case TechnologyKind::scarabDamage: return Scarab_Damage;
        case TechnologyKind::carrierCapacity: return Carrier_Capacity;
        case TechnologyKind::protossGroundWeapons: return Protoss_Ground_Weapons;
        case TechnologyKind::protossGroundArmor: return Protoss_Ground_Armor;
        case TechnologyKind::protossPlasmaShields: return Protoss_Plasma_Shields;
        case TechnologyKind::protossAirWeapons: return Protoss_Air_Weapons;
        case TechnologyKind::protossAirArmor: return Protoss_Air_Armor;
        default: return static_cast<bwgame::UpgradeTypes>(-1);
    }
}

