#include "minecraft.h"
#include <chunk.h>
#include <cstdint>
#include <string>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <stdint.h>
#include <math.h>

// PACKET
void packet::write(uint8_t val){
    buffer[index] = val;
    index++;
}

void packet::write(uint8_t * buf, size_t size){
    memcpy((uint8_t *)(buffer + index), std::move(buf), size);
    index += size;
}

void packet::serverWriteVarInt(int32_t value){
    do {
        uint8_t temp = (uint8_t)(value & 0b01111111);
        value = lsr(value,7);
        if (value != 0) {
            temp |= 0b10000000;
        }
        send(S, &temp, sizeof(temp), 0);
    } while (value != 0);
}

void packet::writePacket(){
	k_mutex_lock(mtx, K_FOREVER);
    serverWriteVarInt(index);
	send(S, buffer, index, 0);
	k_mutex_unlock(mtx);
}

// SERVERBOUND LOGIN
uint8_t minecraft::player::readHandShake(){
    readVarInt(); // length
    int id = readVarInt(); // packet id
    int protocol_version = readVarInt();
    readString(); // we don't need our name
    readUnsignedShort();
    int state = readVarInt();
    if(id != 0){
        return false;
    } else if(protocol_version != 754){
        logerr("wrong protocol version, log in with 1.16.5");
        return false;
    }

    if(state != 1 && state != 2) {
        logerr("wrong state");
        return 0;
    } else {
        return state;
    }
}

bool minecraft::player::readLoginStart(){
    readVarInt(); // length
    int id = readVarInt();
    if(id != 0){
        return false;
    }
    username = readString();
    loginfo("player logging in as " + username);
    return true;
}

uint64_t minecraft::player::readPing(){
    readVarInt(); // length
    readVarInt(); // packet id
    uint64_t payload = readLong(); // payload
    login("ping " + std::to_string((uint32_t)payload));
    return payload;
}

void minecraft::player::readRequest(){
    readVarInt();
    readVarInt();
    login("request packet received");
}

// SERVERBOUND PLAY PACKETS
void minecraft::player::readChat(){
   std::string m = readString();
    login("<" + username + "> " + m);
    if(m == "/stats"){
    } else {
        mc->broadcastChatMessage(m, username);
    }
}

void minecraft::player::readPosition(){
    x = readDouble();
    y = readDouble();
    z = readDouble();
    on_ground = readBool();
    mc->broadcastPlayerPosAndLook(x, y, z, yaw_i, pitch_i, on_ground, id);
    // login("player pos " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z));
}

void minecraft::player::readRotation(){
    yaw = readFloat();
    pitch = readFloat();
    yaw_i = floor(fmap(yaw, 0, 360, 0, 256));
    pitch_i = floor(fmap(pitch, 0, 360, 0, 256));
    on_ground = readBool();
    mc->broadcastPlayerRotation(yaw_i, pitch_i, on_ground, id);
    // login("player rotation " + std::to_string(yaw) + " " + std::to_string(pitch));
}

void minecraft::player::readKeepAlive(){
    login("keepalive received: " + std::to_string((long)readLong()));
}

void minecraft::player::readPositionAndLook(){
    x = readDouble();
    y = readDouble();
    z = readDouble();
    yaw = readFloat();
    pitch = readFloat();
    yaw_i = floor(fmap(yaw, 0, 360, 0, 256));
    pitch_i = floor(fmap(pitch, 0, 360, 0, 256));
    on_ground = readBool();
    mc->broadcastPlayerPosAndLook(x, y, z, yaw_i, pitch_i, on_ground, id);
    // login("player rotation " + std::to_string(yaw) + " " + std::to_string(pitch));
}

void minecraft::player::readTeleportConfirm(){
    readVarInt();
    login("teleport confirm");
}

void minecraft::player::readAnimation(){
    mc->broadcastEntityAnimation(readVarInt(), id);  
}

void minecraft::player::readEntityAction(){
    readVarInt(); // we don't need our own id lmao
    mc->broadcastEntityAction(readVarInt(), id);
    readVarInt(); // we don't need horse jump boost
}

// CLIENTBOUND BROADCAST
void minecraft::broadcastChatMessage(std::string msg, std::string username){
    for(auto player : players){
        if(player.connected){
            player.writeChat(msg, username);
        }
    }
}

void minecraft::broadcastSpawnPlayer(){
    for(auto player : players){
        if(player.connected){
            for(auto p : players){
                if(p.id != player.id && p.connected){
                    player.writeSpawnPlayer(p.x, p.y, p.z, p.yaw_i, p.pitch_i, p.id);
                    player.writeEntityLook(p.yaw_i, p.id);
                }
            }
        }
    }
}

void minecraft::broadcastPlayerPosAndLook(double x, double y, double z, int _yaw_i, int _pitch_i, bool on_ground, uint8_t id){
    for(auto player : players){
        if(player.connected && player.id != id){
            player.writeEntityTeleport(x, y, z, _yaw_i, _pitch_i, on_ground, id);
            player.writeEntityLook(_yaw_i, id);
        }
    }
}

void minecraft::broadcastPlayerRotation(int _yaw_i, int _pitch_i, bool on_ground, uint8_t id){
    for(auto player : players){
        if(player.connected && player.id != id){
            player.writeEntityRotation(_yaw_i, _pitch_i, on_ground, id);
            player.writeEntityLook(_yaw_i, id);
        }
    }
}

void minecraft::broadcastEntityAnimation(uint8_t anim, uint8_t id){
    for(auto player : players){
        if(player.connected && player.id != id){
            player.writeEntityAnimation(anim, id);
        }
    }
}

void minecraft::broadcastEntityAction(uint8_t action, uint8_t id){
    for(auto player : players){
        if(player.connected && player.id != id){
            player.writeEntityAction(action, id);
        }
    }
}

void minecraft::broadcastEntityDestroy(uint8_t id){
    for(auto player : players){
        if(player.connected && player.id != id){
            player.writeEntityDestroy(id);
        }
    }
}

void minecraft::broadcastPlayerInfo(){
    // calculate data length in a horrible non-automated way for now TODO
    uint32_t num = getPlayerNum();
    uint32_t len = 3 + (21 * num);
    for(auto player : players){
        if(player.connected){
            len += player.username.length();
        }
    }
    // broadcast playerinfo
    for(auto player : players){
        if(player.connected){
            packet pac(player.S, player.mtx);
            pac.writeVarInt(0x32);
            pac.writeVarInt(0); // action add player
            pac.writeVarInt(num); // number of players
            for(auto p : players){
                if(p.connected){
                    pac.writeUUID(p.id); // first player's uuid
                    pac.writeString(p.username);
                    pac.writeVarInt(0); // no properties given
                    pac.writeVarInt(1); // gamemode
                    pac.writeVarInt(100); // hardcoded ping TODO
                    pac.writeBoolean(0); // has display name
                }
            }
            pac.writePacket();
            player.login("player info sent");
        }
    }
}

uint8_t minecraft::getPlayerNum(){
    uint8_t i = 0;
    for(auto player : players){
        if(player.connected) i++;
    }
    return i;
}

// CLIENTBOUND PLAYER
void minecraft::player::writeChat(std::string msg, std::string username){
    packet p(S, mtx);
    std::string s = "{\"text\": \"<" + username + "> " + msg + "\",\"bold\": \"false\"}";
    p.writeVarInt(0x0E);
    p.writeString(s);
    p.writeByte(0);
    p.writeUUID(id);
    p.writePacket();
}

void minecraft::player::writeLoginSuccess(){
    packet p(S, mtx);
    p.writeVarInt(0x02);
    p.writeUUID(id);
    p.writeString(username);
    p.writePacket();
    logout("login success sent");
}

void minecraft::player::writeChunk(uint8_t x, uint8_t y){
    packet p(S, mtx);
    p.writeVarInt(0x20); 
    p.writeInt(x); // X
    p.writeInt(y); // Z
    p.writeBoolean(1); // full chunk yes
    p.writeVarInt(0x01); //bitmask set to 0xFF because we're sending the whole chunk

    p.write(height_map_NBT, sizeof(height_map_NBT) / sizeof(height_map_NBT[0]));

    uint8_t b[1024];
    memset(b, 127, 1024);
    p.writeVarInt(1024); // array length 2 bytes as varint
    p.write(b, 1024); // 127 = void biome
    
    p.writeVarInt(4487); // magic 

    p.writeShort(1); // non-air blocks (can't be bothered calculating it and the client doesn't need it)
    p.writeUnsignedByte(8); // bits per block
    p.writeVarInt(256); // palette length 8 bits per block
    p.write(palette, 384); // write palette
    p.writeVarInt(512); // we're sending 512 longs or 4096 bytes
    uint8_t * buf = (uint8_t*)chunk[x][y];
    p.write(buf, 4096);

    p.writeVarInt(0); // no block entities
    p.writePacket();
    logout("chunk sent");
}

void minecraft::player::writePlayerPositionAndLook(double x, double y, double z, float _yaw, float _pitch, uint8_t flags){
    packet p(S, mtx);
    p.writeVarInt(0x34);
    p.writeDouble(x);
    p.writeDouble(y);
    p.writeDouble(z);
    p.writeFloat(_yaw);
    p.writeFloat(_pitch);
    p.writeUnsignedByte(flags);
    p.writeVarInt(0x55);
    p.writePacket();
    logout("player position and look sent");
}

void minecraft::player::writeKeepAlive(){
    packet p(S, mtx);
    p.writeVarInt(0x1F);
    uint32_t num = k_uptime_get() / 1000;
    p.writeLong(num);
    logout("keepalive sent: " + std::to_string(num));
    p.writePacket();
}

void minecraft::player::writeServerDifficulty(){
    packet p(S, mtx);
    p.writeVarInt(0x0D);
    p.writeUnsignedByte(0);
    p.writeBoolean(1);
    logout("server difficulty packet sent");
    p.writePacket();
}

void minecraft::player::writeSpawnPlayer(double x, double y, double z, int _yaw_i, int _pitch_i, uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x04);
    p.writeVarInt(id); // player id
    p.writeUUID(id); // player uuid
    p.writeDouble(x); // player x
    p.writeDouble(y); // player y
    p.writeDouble(z); // player z
    p.writeUnsignedByte(_yaw_i); // player yaw
    p.writeUnsignedByte(_pitch_i); // player pitch
    p.writePacket();
    logout("spawn player sent id:" + std::to_string(id));
}

void minecraft::player::writeJoinGame(){
    packet p(S, mtx);
    p.writeVarInt(0x24);
    p.writeInt(id); // entity id
    p.writeBoolean(0); // is hardcore
    p.writeUnsignedByte(1); // gamemode
    p.writeByte(-1); // previous gamemode
    p.writeVarInt(1); // only one world
    p.writeString("minecraft:overworld"); // only one world
    p.write(dimension_codec_NBT, sizeof(dimension_codec_NBT) / sizeof(dimension_codec_NBT[0])); // NBT with world settings
    p.write(dimension_NBT, sizeof(dimension_NBT) / sizeof(dimension_NBT[0])); // NBT with world settings
    p.writeString("minecraft:overworld"); // spawn world
    p.writeLong(0); // hashed seed
    p.writeVarInt(10); // max players
    p.writeVarInt(12); // view distance
    p.writeBoolean(0); // reduced debug info
    p.writeBoolean(0); // enable respawn screen
    p.writeBoolean(0); // is debug world
    p.writeBoolean(1); // is flat
    logout("join game packet sent");
    p.writePacket();
}

void minecraft::player::writeResponse(){
    packet p(S, mtx);
    p.writeVarInt(0);
    // In the below line, if there is an error from the favicon being too large in size, consider compressing it or increasing "buffer" from 6000 in minecraft.h.
    p.writeString("{\"version\": {\"name\": \"1.16.5\",\"protocol\": 754},\"players\": {\"max\": 5,\"online\": 5,\"sample\": [{\"name\": \"L_S___S_S_S__S_L\",\"id\": \"00000000-0000-0000-0000-000000000000\"},{\"name\": \"L_SS__S_S_S_S__L\",\"id\": \"00000000-0000-0000-0000-000000000001\"},{\"name\": \"L_S_S_S_S_SS___L\",\"id\": \"00000000-0000-0000-0000-000000000002\"},{\"name\": \"L_S__SS_S_S_S__L\",\"id\": \"00000000-0000-0000-0000-000000000003\"},{\"name\": \"L_S___S_S_S__S_L\",\"id\": \"00000000-0000-0000-0000-000000000004\"}]},\"description\": {\"text\": \"A Minecraft server running on an ESP32!\"},\"favicon\":\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAMAAACdt4HsAAABU1BMVEUAAAAWMxcBAwELHwwBBAECCgMMIQ0AAgABAgEBBgEBBAEBBQEAAwECCAIDEAQDDgQDDgQDDAQLIAsQKhIZPRsBCAEBBgEDDgMIGwgGFgcKHwsUMhQDDwMGFwYCCAICCQIGFgcCCQMIHAkEEAQLIwwFEwUKHAoDDAMBAwEDDgMCCQIFEwUCCgIIHAkCCgIDCgMMJA0IGgkRLBISLBIEEQQBBQEBBgEDEAMEEAQBBAEFEwUDCwMHGggIGwgCCgMIGwgDDARMr1AAAAABBAFLrk9FoUlJqU1Bl0QjWiYHHAdKqk5Goko5hjw2gTkfTyEVOxYLJwwCDAI1fjg0fDcRMhIOLA9HpUtEnkgzejYtbzAsay4oYysmXyglXSchVSM/lUM6iD0SNhQPLxEIIAkFGAZEn0hDnUcqZywUOBYDEgQ3gjo/k0I+kUE8jkAxdTQbRhwaRhyU8jDPAAAAQXRSTlMABPsT0YwI9/Tr4LTw2tOHZVomEQf0y8ZoQjwN3a+vkYp6dm9dUyH65uW4tpeRc2hVKh4ZzMe+pKCYlSq8u6h/XiA2BHYAAAPMSURBVFjDzZdZVxNBEIU7Y0ICCRokgIiAiOACCIr7Pt+EhCxkIQlLgLAruP//JzM4xMx0JRLP8Rzv43RVddXtqtvT6l/D5/tbz+m+QNh/1wCj2x8O9F3tzHt4vAcPemaHL5rMpQnH28rXdra2dmp5y4kxceki7oEQQLm6tx43HaTX96plgFDgTyF8DyLA4WYybnoQT24eApHJtoW8GgTKKwlTRGKlDAw+bO0/H4RUKW22RLqUgmC0VfovgOWM2RaZZWBWLKMrDKcr5h/x6RTCXYL/U6gkzQsgWYGnWgRfGPJL5oWwlIewt4oXkJfKl4nIw6zbPwoVeX85hwpEXecf5DRpdoDkKcGHTQQMworZEVZg8DcNk7BsdohlmGzMT4RUptMAmRSR88kKQMnsGCW46SQQopzuPEC6zMCvFCbaMZhItOPxlrLRw6FsdbyRO4RUbuNYjn3IjTP9g02xWbZpYFtssk24Xg8wDlIPZY+AgaFnz4ZCQCordRPMnVVQieuLG8DjhdjZnE4NAhu6TfzArmEaPupre2DcUg1MGrCnW32ER6oPslKXGAuqCVMGqSVpmwUVgA/aypZ9Qi48gC3N7IPdS2FIawcAl30ewfFjaSmkYVT5OdEir8Jt5UEfrGqGeS6ru+SESbszojwYCQoTm+OeMoTv5XpgDX72pZ0UbGvfLd4oDfexNMNtLDkAry8eQCrtRC7hRCpBJHEHo9/r32+wI5LoJ6/fXnBNeRCFT0KqfjUmNNJakRsxt3+sl+Ka0Ehj6orUylW7R12om1WlVr6ipqRhWitguYqIWhTWdM2AKfVIHOcvFgS6Gjd3wML60mKcVa8oKFkLLscag4QlSFK8Qq99K8iS9hWMRUf3DfgqmOxCoL58HaqyaD9XDp7Lwl89E1W7hlRConFgUTlYDEkUJlLcOBebVUmy36kGJiTpXz2Xrf4IB94Ujovca5KErh6K694EDoj0N5pkQ5sGbnv0aEcX/ivnc9LNkVvvPsNj5cIT+OzWzSO6GxM3D7V48wF/t/l14brFD5dJrXngfE/cPGZttfXgLWTdDA41CffVEMXdZnqC09oLJthM9W6RAdcj5j0UGjR/02bRofpb45AKmvCPw75DZKZOz4jSMFKnOuMQuA9znmXfGOz/ymHLpkdA9Px2W9+HUZ/2s30fCjYPSf1e+z2USbv+AszEhN/1MSiW4vEcDCsRw5CLx1eLMBoTdxgHaiWYUS0wA6UaMOdTMvpCAEbLR9FVA2DA4V+0GAJetl5/CQy1f8TOd/deavOo7O2+5lP/GX4CF6CJedzJaM4AAAAASUVORK5CYII=\"}");
    logout("response packet sent");
    p.writePacket();
}

void minecraft::player::writePong(uint64_t payload){
    packet p(S, mtx);
    p.writeVarInt(0x01); // packet id
    p.writeLong(payload); // payload
    logout("pong");
    p.writePacket();
}

void minecraft::player::writeEntityTeleport(double x, double y, double z, int _yaw_i, int _pitch_i, bool on_ground, uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x56); // packet id
    p.writeVarInt(id);
    p.writeDouble(x);
    p.writeDouble(y);
    p.writeDouble(z);
    p.writeByte(_yaw_i);
    p.writeByte(_pitch_i);
    p.writeBoolean(on_ground);
    p.writePacket();
}

void minecraft::player::writeEntityRotation(int _yaw_i, int _pitch_i, bool on_ground, uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x29); // packet id
    p.writeVarInt(id);
    p.writeByte(_yaw_i);
    p.writeByte(_pitch_i);
    p.writeBoolean(on_ground);
    p.writePacket();
}

void minecraft::player::writeEntityLook(int _yaw_i, uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x3A); // packet id
    p.writeVarInt(id);
    p.writeByte(_yaw_i);
    p.writePacket();
}

void minecraft::player::writeEntityAnimation(uint8_t anim, uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x05); // packet id
    p.writeVarInt(id);
    switch(anim){
        case 0:
            p.writeByte(0);
            break;
        case 1:
            p.writeByte(3);
            break;
    }
    p.writePacket();
}

void minecraft::player::writeEntityAction(uint8_t action, uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x44); // packet id
    p.writeVarInt(id);
    switch(action){
        case 0:
            p.writeUnsignedByte(6); // field unique id
            p.writeVarInt(18); // we need only poses since swimming etc. isn't supported
            p.writeVarInt(5); // sneak
            break;
        case 1:
            p.writeUnsignedByte(6); // field unique id
            p.writeVarInt(18); // we need only poses since swimming etc. isn't supported
            p.writeVarInt(0); // stand
            break;
    }
    p.writeUnsignedByte(0xFF); // terminate entity metadata array
    p.writePacket();
}

void minecraft::player::writeEntityDestroy(uint8_t id){
    packet p(S, mtx);
    p.writeVarInt(0x36); // packet id
    p.writeVarInt(1); // entity count
    p.writeVarInt(id);
    p.writePacket();
}

// READ TYPES
uint16_t minecraft::player::readUnsignedShort(){
	uint8_t r[sizeof(uint16_t)] = {0};

	recv(S, r, sizeof(uint16_t), 0);

	return sys_get_be16(r);
}

float minecraft::player::readFloat(){
    uint8_t r[sizeof(float)] = {0};

	recv(S, r, sizeof(float), 0);

    return (float)(sys_get_be32(r));
}

double minecraft::player::readDouble(){
    uint8_t r[sizeof(double)] = {0};

	recv(S, r, sizeof(double), 0);

    return (double)(sys_get_be64(r));
}

uint32_t minecraft::player::readUnsignedLong(){
    uint8_t r[sizeof(uint32_t)] = {0};

	recv(S, r, sizeof(uint32_t), 0);

    return (int32_t)(sys_get_be32(r));
}

int64_t minecraft::player::readLong(){
    uint8_t r[sizeof(int64_t)] = {0};

	recv(S, r, sizeof(int64_t), 0);

    return (int64_t)(sys_get_be64(r));
}

std::string minecraft::player::readString(){
    int length = readVarInt();

    std::string result(length + 1, 0);

	recv(S, &result[0], length, 0);

    return result;
}

int32_t minecraft::player::readVarInt() {
	int numRead = 0;
    int32_t result = 0;
    uint8_t current_byte;

    do {
        recv(S, &current_byte, 1, 0);

        int32_t value_segment = (current_byte & 0b01111111);
        result |= (value_segment << (7 * numRead));

        numRead++;
        if (numRead > 5) {
            return -1;
        }
    } while ((current_byte & 0b10000000) != 0);

    return result;
}

uint8_t minecraft::player::readByte(){
	uint8_t r;

	recv(S, &r, sizeof(uint8_t), 0);

    return r;
}

bool minecraft::player::readBool(){
	uint8_t r;

	recv(S, &r, sizeof(uint8_t), 0);

	return (bool)r;
}

// WRITE TYPES
void packet::writeDouble(double value){
    unsigned char * p = reinterpret_cast<unsigned char *>(&value);
    for(int i=7; i>=0; i--){
        write(p[i]);
    }
}

void packet::writeFloat(float value) {
    unsigned char * p = reinterpret_cast<unsigned char *>(&value);
    for(int i=3; i>=0; i--){
        write(p[i]);
    }
}

void packet::writeVarInt(int32_t value) {
    do {
        uint8_t temp = (uint8_t)(value & 0b01111111);
        value = lsr(value,7);
        if (value != 0) {
            temp |= 0b10000000;
        }
        write(temp);
    } while (value != 0);
}

void packet::writeVarLong(int64_t value) {
    do {
        uint8_t temp = (uint8_t)(value & 0b01111111);
        value = lsr(value,7);
        if (value != 0) {
            temp |= 0b10000000;
        }
        write(temp);
    } while (value != 0);
}

void packet::writeString(std::string str){
    int length = str.length();
    uint8_t buf[length + 1]; 
	memcpy(buf, str.data(), length);
    writeVarInt(length);
    write(buf, length);
    /*for(int i=0; i<length; i++){
        write(buf[i]);
    }*/
}

void packet::writeLong(int64_t num){
    for(int i=7; i>=0; i--){
        write((uint8_t)((num >> (i*8)) & 0xff));
    }
}

void packet::writeUnsignedLong(uint64_t num){
    for(int i=7; i>=0; i--){
        write((uint8_t)((num >> (i*8)) & 0xff));
    }
}

void packet::writeUnsignedShort(uint16_t num){
    write((uint8_t)((num >> 8) & 0xff));
    write((uint8_t)(num & 0xff));
}

void packet::writeUnsignedByte(uint8_t num){
    write(num);
}

void packet::writeInt(int32_t num){
    write((uint8_t)((num >> 24) & 0xff));
    write((uint8_t)((num >> 16) & 0xff));
    write((uint8_t)((num >> 8) & 0xff));
    write((uint8_t)(num & 0xff));
}

void packet::writeShort(int16_t num){
    write((uint8_t)((num >> 8) & 0xff));
    write((uint8_t)(num & 0xff));
}

void packet::writeByte(int8_t num){
    write(num);
}

void packet::writeBoolean(uint8_t val){
    write(val);
}

void packet::writeUUID(int user_id){
    uint8_t b[15] = {0};
    write(b, 15);
    write(user_id);
}

void minecraft::player::writeLength(uint32_t length){
    do {
        uint8_t temp = (uint8_t)(length & 0b01111111);
        length = lsr(length,7);
        if (length != 0) {
            temp |= 0b10000000;
        }
		send(S, &temp, sizeof(temp), 0);
    } while (length != 0);
}

// HANDLERS
bool minecraft::player::join(){
    uint8_t res = readHandShake();
    if(res == 1){
        readRequest();
        k_msleep(1000);
        writeResponse();
        uint64_t payload = readPing();
        writePong(payload);
        return false;
    } else if(res == 2){
        if(!readLoginStart()) return false;
        writeLoginSuccess();
    }
    connected = true;
    writeJoinGame();
    writePlayerPositionAndLook(0, 5, 0, 0, 0, 0x00);
    writeServerDifficulty();
    writeChunk(0, 0);
    writeChunk(0, 1);
    writeChunk(1, 0);
    writeChunk(1, 1);
    mc->broadcastPlayerInfo();
    mc->broadcastChatMessage(username + " joined the server", "Server");
    mc->broadcastSpawnPlayer();
    return true;
}

void minecraft::handle(){
    for(auto player : players){
        if(player.connected){
            player.writeKeepAlive();
        }
    }
}

void minecraft::player::handle(){
	uint32_t length = readUnsignedLong();
	uint32_t packetid = readUnsignedLong();
	switch (packetid){
	case 0x03:
		readChat();
		break;
	case 0x12:
		readPosition();
		break;
	case 0x14:
		readRotation();
		break;
	case 0x10:
		readKeepAlive();
		break;
	case 0x13:
		readPositionAndLook();
		break;
	case 0x00:
		readTeleportConfirm();
		break;
	case 0x2C:
		readAnimation();
		break;
	case 0x1C:
		readEntityAction();
		break;
	default:
		//loginfo("id: 0x" + std::to_string(packetid, HEX) + " length: " + std::to_string(length));
		for (int i = 0; i < length - VarIntLength(packetid); i++ ){
			// loginfo("packet id " + std::to_string(packetid));
			readByte();
		}
		break;
	}
}

// UTILITIES
void minecraft::player::loginfo(std::string msg){
    //Serial.println( "[INFO] p" + std::to_string(id) + " " + msg);
}

void minecraft::player::logerr(std::string msg){
    //Serial.println( "[ERROR] p" + std::to_string(id) + " " + msg);
}

void minecraft::player::login(std::string msg){
    //Serial.println( "[INFO] p" + std::to_string(id) + " <- " + msg);
}

void minecraft::player::logout(std::string msg){
    //Serial.println( "[INFO] p" + std::to_string(id) + " -> " + msg);
}

int32_t lsr(int32_t x, uint32_t n){
  return (int32_t)((uint32_t)x >> n);
}

uint32_t minecraft::player::VarIntLength(int val) {
    if(val == 0){
        return 1;
    }
    return (int)floor(log(val) / log(128)) + 1;
}

float fmap(float x, float in_min, float in_max, float out_min, float out_max) {
  return (float)(x - in_min) * (out_max - out_min) / (float)(in_max - in_min) + out_min;
}
