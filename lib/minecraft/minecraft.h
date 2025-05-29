#ifndef MINECRAFT_H
#define MINECRAFT_H

#include <string>
#include <zephyr/kernel.h>
#include <stdint.h>

class packet{
    public:
    uint8_t buffer[6000];
    uint32_t index = 0;
    int S;
	struct k_mutex *mtx;

    packet(int __S, struct k_mutex *_mtx) {
        S = __S;
        mtx = _mtx;
    }

    void write(uint8_t val);
    void write(uint8_t * buf, size_t size);
    void writePacket();

    void writeDouble        (double value);
    void writeFloat         (float value);
    void writeVarInt        (int32_t value);
    void writeVarLong       (int64_t value);
    void writeString        (std::string str);
    void writeUnsignedLong  (uint64_t num);
    void writeUnsignedShort (uint16_t num);
    void writeUnsignedByte  (uint8_t num);
    void writeLong          (int64_t num);
    void writeInt           (int32_t num);
    void writeShort         (int16_t num);
    void writeByte          (int8_t num);
    void writeBoolean       (uint8_t val);
    void writeUUID          (int user_id);

    // write to client
    void serverWriteVarInt  (int32_t value);
};

class minecraft{
    public:
    class player {
        public:
        struct k_mutex *mtx;
        int S;
        minecraft* mc;
        bool connected = false;
		std::string username;
        double x = 0;
        double y = 5;
        double z = 0;
        double yaw = 0;
        double pitch = 0;
        int yaw_i = 0;
        int pitch_i = 0;
        bool on_ground = true;
        float health = 0;
        uint8_t food = 0;
        float food_sat = 0;
        uint8_t id = 0;

		player() { // Initialize mtx to nullptr
			mtx = (struct k_mutex *)k_malloc(sizeof(struct k_mutex));

			if (mtx == NULL) {
				// Allocation failed. Handle the error appropriately.
				// For this example, we'll print an error message.
				// In a real application, you might set an error flag,
				// throw an exception (if using C++ exceptions), or take other actions.
				//printk("Error: Failed to allocate memory for k_mutex in Player constructor.\n");
				// mtx remains nullptr, indicating the object is not properly initialized.
				return;
			}

			// Step 3: Initialize the mutex using k_mutex_init
			// k_mutex_init takes a pointer to the k_mutex structure.
			int ret = k_mutex_init(mtx);

			// Step 4: Check if k_mutex_init was successful
			// k_mutex_init returns 0 on success, and a negative error code on failure.
			if (ret != 0) {
				// Initialization failed. Handle the error.
				//printk("Error: Failed to initialize k_mutex in Player constructor. Error code: %d\n", ret);
				// Since initialization failed, free the previously allocated memory.
				mtx = nullptr; // Set to nullptr to indicate failure.
			} else {
				//printk("Player mutex allocated and initialized successfully.\n");
			}
		}

		~player() {
			if (mtx != nullptr) {
				mtx = nullptr;
			}
		}

        bool join               ();
        void handle             ();

        uint8_t readHandShake   ();
        bool readLoginStart     ();
        uint64_t readPing       ();
        void readRequest        ();

        void readChat           ();
        void readPosition       ();
        void readRotation       ();
        void readKeepAlive      ();
        void readPositionAndLook();
        void readTeleportConfirm();
        void readAnimation      ();
        void readEntityAction   ();

        void writeResponse      ();
        void writeLoginSuccess  ();
        void writeChunk         (uint8_t x, uint8_t y);
        void writePlayerPositionAndLook(double x, double y, double z, float yaw, float pitch, uint8_t flags);
        void writeKeepAlive     ();
        void writeServerDifficulty();
        void writeSpawnPlayer   (double x, double y, double z, int yaw, int pitch, uint8_t id);
        void writeJoinGame      ();
        void writePong          (uint64_t payload);
        void writeChat          (std::string msg, std::string username);
        void writeEntityTeleport(double x, double y, double z, int yaw, int pitch, bool on_ground, uint8_t id);
        void writeEntityRotation(int yaw, int pitch, bool on_ground, uint8_t id);
        void writeEntityLook    (int yaw, uint8_t id);
        void writeEntityAnimation(uint8_t anim, uint8_t id);
        void writeEntityAction  (uint8_t action, uint8_t id);
        void writeEntityDestroy (uint8_t id);

        void loginfo            (std::string msg);
        void logerr             (std::string msg);
        void login              (std::string msg);
        void logout             (std::string msg);

        float readFloat         ();
        double readDouble       ();
        int32_t readVarInt      ();
        std::string readString       ();
        int64_t readLong        ();
        uint32_t readUnsignedLong();
        uint16_t readUnsignedShort();
        uint32_t VarIntLength   (int val);
        uint8_t readByte        ();
        bool readBool           ();

        void writeLength        (uint32_t length);
    };

    uint64_t tick = 0;
    uint64_t prev_keepalive = 0;
    player players[5];

    void handle                      ();
    void broadcastChatMessage        (std::string msg, std::string username);
    void broadcastSpawnPlayer        ();
    void broadcastPlayerPosAndLook   (double x, double y, double z, int yaw, int pitch, bool on_ground, uint8_t id);
    void broadcastPlayerInfo         ();
    void broadcastPlayerRotation     (int yaw, int pitch, bool on_ground, uint8_t id);
    void broadcastEntityAnimation    (uint8_t anim, uint8_t id);
    void broadcastEntityAction       (uint8_t action, uint8_t id);
    void broadcastEntityDestroy      (uint8_t id);
    uint8_t getPlayerNum             ();
};

int32_t lsr(int32_t x, uint32_t n);
float fmap(float x, float in_min, float in_max, float out_min, float out_max);

#endif
