#include "macaroons_shim.hpp"

/**
 * Variables to test Macaroons
 *
 * In an actual implementation, the Macaroon will be generated by
 * the server (resource owner) and provided to the client (resource user).
 * */
static std::string key_;
// static std::string id = "id for a bad secret";
// static std::string location = "https://www.modbus.com/macaroons/";
// static std::string expected_signature =
//     "27c9baef16ae041625139857bfca2cebebdcba4ce6637c59ea2693107cf053ce";

// static int16_t default_address_min_caveat = 0x0000;
// static int16_t default_address_max_caveat = 0xFFFF;
static std::string default_function_caveat = "READ-ONLY";
static std::vector<int> default_function_caveats = {MODBUS_FC_READ_COILS, MODBUS_FC_WRITE_SINGLE_COIL, MODBUS_FC_WRITE_MULTIPLE_COILS};
static macaroons::Macaroon client_macaroon_;
static macaroons::Macaroon server_macaroon_;

/******************
 * HELPER FUNCTIONS
 *****************/

/**
 * Three functions to create_function_caveat
 * In all cases, a string representation of a bitfield is returned
 * 1. Input: READ-ONLY or WRITE-ONLY
 * 2. Input: Single int function code
 * 3. Input: Vector of ints of multiple function codes
 * */
std::string
create_function_caveat_common(int fc) {
    return "function = " + std::to_string(fc);
}

std::string
create_function_caveat(std::string function_code) {
    uint32_t fc = 0;

    if(function_code == "READ-ONLY") {
        fc |= 1<<MODBUS_FC_READ_COILS;
        fc |= 1<<MODBUS_FC_READ_DISCRETE_INPUTS;
        fc |= 1<<MODBUS_FC_READ_HOLDING_REGISTERS;
        fc |= 1<<MODBUS_FC_READ_INPUT_REGISTERS;
        fc |= 1<<MODBUS_FC_READ_EXCEPTION_STATUS;
        fc |= 1<<MODBUS_FC_REPORT_SLAVE_ID;
        fc |= 1<<MODBUS_FC_READ_STRING;
    } else if(function_code == "WRITE-ONLY") {
        fc |= 1<<MODBUS_FC_WRITE_SINGLE_COIL;
        fc |= 1<<MODBUS_FC_WRITE_SINGLE_REGISTER;
        fc |= 1<<MODBUS_FC_WRITE_MULTIPLE_COILS;
        fc |= 1<<MODBUS_FC_WRITE_MULTIPLE_REGISTERS;
        fc |= 1<<MODBUS_FC_MASK_WRITE_REGISTER;
        fc |= 1<<MODBUS_FC_WRITE_STRING;
    } else {
        return "";
    }

    return create_function_caveat_common(fc);
}

std::string
create_function_caveat(int function_code) {
    return create_function_caveat_common(1<<function_code);
}

std::string
create_function_caveat(std::vector<int> function_codes) {
    uint32_t fc = 0;
    for (int code : function_codes) {
        fc |= 1<<code;
    }
    return create_function_caveat_common(fc);
}

/**
 * Verifies that the function caveats are not mutually exclusive
 * e.g., that we don't have both READ-ONLY and WRITE-ONLY
 * */
bool
check_function_caveats(std::vector<std::string> first_party_caveats)
{
    uint32_t fc = 0xFFFFFFFF;
    std::string function_token = "function = ";
    for(std::string caveat : first_party_caveats) {
        if(caveat.find(function_token) == 0) {
            fc &= std::stoi(caveat.substr(function_token.size()));
        }
    }

    if(fc) {
        return true;
    } else {
        return false;
    }
}

/**
 * Create an address caveat
 *
 * address = 0xABCDEFGH
 * ABCD is the min address
 * EFGH is the max address
 * */
std::string
create_address_caveat(uint16_t min, uint16_t max)
{
    uint32_t ac;
    ac = (min<<16) + max;
    return "address = " + std::to_string(ac);
}

/**
 * Verifies that the addresses in the request are not excluded by
 * address caveats
 * */
bool
check_address_caveats(std::vector<std::string> first_party_caveats, std::string address_request)
{
    std::string address_token = "address = ";

    /* convert address request to an int */
    uint32_t ar = std::stoi(address_request.substr(address_token.size()));
    uint16_t ar_min = (0xFFFF0000 & ar)>>16;
    uint16_t ar_max = 0x0000FFFF & ar;

    uint32_t ac;
    uint16_t ac_min;
    uint16_t ac_max;

    /**
     * iterate through all address caveats, extract min and max
     *
     * if the requested address min and max are outside the bounds
     * of any caveat, return false.  otherwise return true.
     * */
    for(std::string caveat : first_party_caveats) {
        if(caveat.find(address_token) == 0) {
            ac = std::stoi(caveat.substr(address_token.size()));
            ac_min = (0xFFFF0000 & ac)>>16;
            ac_max = 0x0000FFFF & ac;

            if(ar_min < ac_min || ar_max > ac_max) {
                return false;
            }
        }
    }

    return true;
}

/**
 * Based on function, address, and number, calculate
 * the maximum address expected to be accessed.
 *
 * For bitwise operations (e.g., read_bits), round up
 * to the nearest byte
 * */
uint16_t
find_max_address(int function, uint16_t addr, int nb)
{
    uint16_t addr_max;

    switch(function) {
        case MODBUS_FC_READ_COILS:
        case MODBUS_FC_WRITE_MULTIPLE_COILS:
        case MODBUS_FC_READ_DISCRETE_INPUTS:
            addr_max = addr + (nb / 8) + ((nb % 8) ? 1 : 0);
            break;
        case MODBUS_FC_READ_HOLDING_REGISTERS:
        case MODBUS_FC_READ_INPUT_REGISTERS:
        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
        case MODBUS_FC_WRITE_AND_READ_REGISTERS:
            addr_max = addr + (nb * 2);
            break;
        case MODBUS_FC_WRITE_SINGLE_COIL:
        case MODBUS_FC_REPORT_SLAVE_ID:
        case MODBUS_FC_READ_EXCEPTION_STATUS:
            addr_max = addr;
            break;
        case MODBUS_FC_WRITE_SINGLE_REGISTER:
        case MODBUS_FC_MASK_WRITE_REGISTER:
            addr_max = addr + 2;
            break;
        case MODBUS_FC_WRITE_STRING:
            addr_max = addr + nb;
            break;
        default:
            addr_max = addr;
    }

    return addr_max;
}

/******************
 * CLIENT FUNCTIONS
 *****************/
int
initialise_client_macaroon(modbus_t *ctx)
{
    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    int rc;
    uint8_t *tab_rp_string;

    std::string serialised;

    /* Allocate and initialize the memory to store the string */
    tab_rp_string = (uint8_t *)malloc(MODBUS_MAX_STRING_LENGTH * sizeof(uint8_t));
    memset(tab_rp_string, 0, MODBUS_MAX_STRING_LENGTH * sizeof(uint8_t));

    rc = modbus_read_string(ctx, tab_rp_string);

    serialised = std::string((char *)tab_rp_string);

    if(rc == (int)serialised.size()) {
        // try to deserialise the string into a Macaroon
        try {
            client_macaroon_ = macaroons::Macaroon::deserialize(serialised);
        } catch(macaroons::exception::Invalid &e) {
            std::cout << e.what() << std::endl;
        }

        if(client_macaroon_.is_initialized()){
            return 1;
        }
    }

    return -1;
}

bool
send_macaroon(modbus_t *ctx, int function, uint16_t addr, int nb)
{
    int rc;
    macaroons::Macaroon temp_macaroon;

    if(!client_macaroon_.is_initialized()) {
        std::cout << "> " << "Macaroon not initialised" << std::endl;
        return false;
    }

    /* add the function as a caveat to a temporary Macaroon*/
    temp_macaroon = client_macaroon_.add_first_party_caveat(create_function_caveat(function));

    /* add the address range as a caveat to a temporary Macaroon*/
    uint16_t addr_max = find_max_address(function, addr, nb);
    temp_macaroon = temp_macaroon.add_first_party_caveat(create_address_caveat(addr, addr_max));

    /* serialise the Macaroon and send it to the server */
    std::cout << "> " <<  "sending Macaroon" << std::endl;
    std::cout << temp_macaroon.inspect() << std::endl;
    std::cout << display_marker << std::endl;

    std::string serialised = temp_macaroon.serialize();
    rc = modbus_write_string(ctx, (uint8_t *)serialised.c_str(), (int)serialised.length());

    std::cout << display_marker << std::endl;
    if(rc == (int)serialised.length()) {
        std::cout << "> " <<  "Macaroon response received" << std::endl;
        return true;
    } else {
        std::cout << "> " << "Macaroon response failed" << std::endl;
        return false;
    }
}

/**
 * Shim for modbus_read_bits()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_READ_COILS command
 * 2. Reads the boolean status of bits and sets the array elements
 *    in the destination to TRUE or FALSE (single bits)
 * */
int
modbus_read_bits_macaroons(modbus_t *ctx, int addr, int nb, uint8_t *dest)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_READ_COILS, addr, nb)) {
        std::cout << "> " <<  "calling modbus_read_bits()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_read_bits(ctx, addr, nb, dest);
    }

    return -1;
}

/**
 * Shim for modbus_read_input_bits()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_READ_DISCRETE_INPUTS command
 * 2. Same as modbus_read_bits but reads the remote device input table
 * */
int
modbus_read_input_bits_macaroons(modbus_t *ctx, int addr, int nb, uint8_t *dest)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_READ_DISCRETE_INPUTS, addr, nb)) {
        std::cout << "> " << "calling modbus_read_input_bits()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_read_input_bits(ctx, addr, nb, dest);
    }

    return -1;
}

/**
 * Shim for modbus_read_registers()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_READ_HOLDING_REGISTERS command
 * 2. Reads the holding registers of remote device and put the data into an
 *    array
 * */
int
modbus_read_registers_macaroons(modbus_t *ctx, int addr, int nb, uint16_t *dest)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_READ_HOLDING_REGISTERS, addr, nb)) {
        std::cout << "> " << "calling modbus_read_registers()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_read_registers(ctx, addr, nb, dest);
    }

    return -1;
}

/**
 * Shim for modbus_read_input_registers()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_READ_INPUT_REGISTERS command
 * 2. Reads the holding registers of remote device and put the data into an
 *    array
 * */
int
modbus_read_input_registers_macaroons(modbus_t *ctx, int addr, int nb, uint16_t *dest)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_READ_INPUT_REGISTERS, addr, nb)) {
        std::cout << "> " << "calling modbus_read_input_registers()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_read_input_registers(ctx, addr, nb, dest);
    }

    return -1;
}

/**
 * Shim for modbus_write_bit()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_WRITE_SINGLE_COIL command
 * 2. Turns ON or OFF a single bit of the remote device
 * */
int
modbus_write_bit_macaroons(modbus_t *ctx, int addr, int status)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_WRITE_SINGLE_COIL, addr, 0)) {
        std::cout << "> " << "calling modbus_write_bit()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_write_bit(ctx, addr, status);
    }

    return -1;
}

/**
 * Shim for modbus_write_register()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_WRITE_SINGLE_REGISTER command
 * 2. Writes a value in one register of the remote device
 * */
int
modbus_write_register_macaroons(modbus_t *ctx, int addr, const uint16_t value)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_WRITE_SINGLE_REGISTER, addr, 0)) {
        std::cout << "> " << "calling modbus_write_register()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_write_register(ctx, addr, value);
    }

    return -1;
}

/**
 * Shim for modbus_write_bits()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_WRITE_SINGLE_COIL command
 * 2. Write the bits of the array in the remote device
 * */
int
modbus_write_bits_macaroons(modbus_t *ctx, int addr, int nb, const uint8_t *src)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_WRITE_MULTIPLE_COILS, addr, nb)) {
        std::cout << "> " << "calling modbus_write_bits()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_write_bits(ctx, addr, nb, src);
    }

    return -1;
}

/**
 * Shim for modbus_write_registers()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_WRITE_MULTIPLE_REGISTERS command
 * 2. Write the values from the array to the registers of the remote device
 * */
int
modbus_write_registers_macaroons(modbus_t *ctx, int addr, int nb, const uint16_t *data)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_WRITE_MULTIPLE_REGISTERS, addr, nb)) {
        std::cout << "> " << "calling modbus_write_registers()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_write_registers(ctx, addr, nb, data);
    }

    return -1;
}

/**
 * Shim for modbus_mask_write_register()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_MASK_WRITE_REGISTER command
 * 2. I'm not actually sure what this does...
 *    The unit test appears designed to fail
 * */
int
modbus_mask_write_register_macaroons(modbus_t *ctx, int addr,
                                     uint16_t and_mask, uint16_t or_mask)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_MASK_WRITE_REGISTER, addr, 0)) {
        std::cout << "> " << "calling modbus_mask_write_register()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_mask_write_register(ctx, addr, and_mask, or_mask);
    }

    return -1;
}

/**
 * Shim for modbus_write_and_read_registers()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_WRITE_AND_READ_REGISTERS command
 * 2. Write multiple registers from src array to remote device and read multiple
 *    registers from remote device to dest array
 * */
int
modbus_write_and_read_registers_macaroons(modbus_t *ctx, int write_addr,
                                          int write_nb, const uint16_t *src,
                                          int read_addr, int read_nb,
                                          uint16_t *dest)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    /**
     * send_macaroon() will create an address range caveat
     * we need to find the entire range that this function is trying to access
     * since there's no way to have disjoint caveats
     * */
    uint16_t write_addr_max = find_max_address(MODBUS_FC_WRITE_AND_READ_REGISTERS, write_addr, write_nb);
    uint16_t read_addr_max = find_max_address(MODBUS_FC_WRITE_AND_READ_REGISTERS, read_addr, read_nb);
    uint16_t addr = (write_addr < read_addr) ? write_addr : read_addr;
    int nb = ((write_addr < read_addr) ? (read_addr_max - write_addr) : (write_addr_max - read_addr)) / 2;

    if(send_macaroon(ctx, MODBUS_FC_WRITE_AND_READ_REGISTERS, addr, nb)) {
        std::cout << "> " << "calling modbus_write_and_read_registers()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_write_and_read_registers(ctx, write_addr, write_nb, src,
                                               read_addr, read_nb, dest);
    }

    return -1;
}

/**
 * Shim for modbus_report_slave_id()
 *
 * 1. Sends a Macaroon with the MODBUS_FC_REPORT_SLAVE_ID command
 * 2. Send a request to get the slave ID of the device (only available in
 *    serial communication)
 * */
int
modbus_report_slave_id_macaroons(modbus_t *ctx, int max_dest,
                                           uint8_t *dest)
{
    std::string command;

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    if(send_macaroon(ctx, MODBUS_FC_REPORT_SLAVE_ID, 0, 0)) {
        std::cout << "> " << "calling modbus_report_slave_id()" << std::endl;
        std::cout << display_marker << std::endl;

        return modbus_report_slave_id(ctx, max_dest, dest);
    }

    return -1;
}

/* Receive the request from a modbus master */
int
modbus_receive_macaroons(modbus_t *ctx, uint8_t *req)
{
    return modbus_receive(ctx, req);
}

/******************
 * SERVER FUNCTIONS
 *****************/

int
initialise_server_macaroon(std::string location, std::string key, std::string id)
{
    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    server_macaroon_= macaroons::Macaroon(location, key, id);
    key_ = key;

    if(server_macaroon_.is_initialized()){
        return 1;
    }

    return -1;
}

/**
 * Process an incoming Macaroon:
 * 1. Deserialise a string
 * 2. Check if it's a valid Macaroon
 * 3. Perform verification on the Macaroon
 *
 * TODO:  Figure out how to handle write_and_read_registers
 * */
bool
process_macaroon(uint8_t *tab_string, int function, uint16_t addr, int nb)
{
    std::string serialised = std::string((char *)tab_string);

    std::string fc = create_function_caveat(function);
    bool function_as_caveat = false;

    uint16_t ar_max = find_max_address(function, addr, nb);
    std::string ar = create_address_caveat(addr, ar_max);
    bool address_as_caveat = false;

    macaroons::Macaroon M;
    macaroons::Verifier V;

    // try to deserialise the string into a Macaroon
    try {
        M = macaroons::Macaroon::deserialize(serialised);
    } catch(macaroons::exception::Invalid &e) {
        std::cout << e.what() << std::endl;
    }

    if(M.is_initialized()){
        /**
         * - Confirm the fpcs aren't mutually exclusive (e.g., READ-ONLY and WRITE-ONLY)
         * - Confirm requested addresses are not out of range (based on caveats)
         * - Add all first party caveats to the verifier
         * - Confirm that the requested function is one of the first party caveats
         * - Confirm that the requested address range is one of the first party caveats
         * - Verify the Macaroon
         * */
        // extract all fpcs
        std::vector<std::string> first_party_caveats = M.first_party_caveats();
        // functions: perform mutual exclusion check
        if(check_function_caveats(first_party_caveats)) {
            // addresses: perform range check
            if(check_address_caveats(first_party_caveats, ar)) {
                for(std::string first_party_caveat : first_party_caveats) {
                    // add fpcs to verifier
                    V.satisfy_exact(first_party_caveat);
                    // check if the requested function is a caveat
                    if(first_party_caveat == fc) {
                        function_as_caveat = true;
                    } else if(first_party_caveat == ar) {
                        address_as_caveat = true;
                    }
                }
                // confirm the requested function is a caveat
                if(function_as_caveat) {
                    // confirm the requested addresses is a caveat
                    if(address_as_caveat) {
                        // perform verification
                        if(V.verify_unsafe(M, key_)) {
                            std::cout << "> " << "Macaroon verification: PASS" << std::endl;
                            return true;
                        } else {
                            std::cout << "> " << "Macaroon verification: FAIL" << std::endl;
                        }
                    } else {
                        std::cout << "> " << "Address range not protected as a Macaroon caveat" << std::endl;
                    }
                } else {
                    std::cout << "> " << "Function not protected as a Macaroon caveat" << std::endl;
                }
            } else {
                std::cout << "> " << "Requested addresses are out of range" << std::endl;
            }
        } else {
            std::cout << "> " << "Function caveats are mutually exclusive" << std::endl;
        }

    } else {
        std::cout << "> " << "Macaroon verification: " <<
            "MACAROON NOT INITIALISED" << std::endl;
    }

    return false;
}

/**
 * Analyses the request and constructs a response.
 *
 * If an error occurs, this function construct the response
 * accordingly.
 * */
int
modbus_process_request_macaroons(modbus_t *ctx, uint8_t *req,
                                 int req_length, uint8_t *rsp, int *rsp_length,
                                 modbus_mapping_t *mb_mapping,
                                 shim_t shim_type, shim_s shim_state)
{
    int *offset = (int *)malloc(sizeof(int));
    int *slave_id = (int *)malloc(sizeof(int));
    int *function = (int *)malloc(sizeof(int));
    uint16_t *addr = (uint16_t *)malloc(sizeof(uint16_t));
    int *nb = (int *)malloc(sizeof(int));
    uint16_t *addr_wr = (uint16_t *)malloc(sizeof(uint16_t));  // only for write_and_read_registers
    int *nb_wr = (int *)malloc(sizeof(int));    // only for write_and_read_registers

    print_shim_info("macaroons_shim", std::string(__FUNCTION__));

    /* get the function from the request */
    modbus_decompose_request(ctx, req, offset, slave_id, function, addr, nb, addr_wr, nb_wr);

    /**
     * If the function is WRITE_STRING we reset tab_string
     * If the function is READ_STRING, skip verification
     * If the function is anything else, we verify the Macaroon
     *
     * In both cases, we then call cheri_macaroons_shim:modbus_process_request()
     * */
    if(*function == MODBUS_FC_WRITE_STRING) {
        /**
         * Zero out the state variable where the Macaroon string is stored
         * then continue to process the request
         * */
        memset(mb_mapping->tab_string, 0, MODBUS_MAX_STRING_LENGTH * sizeof(uint8_t));
    } else if(*function == MODBUS_FC_READ_STRING) {
        /**
         * Serialise the server Macaroon and feed it into tab_string
         *
         * If uninitialised, zero out tab_string
         * */
        if(server_macaroon_.is_initialized()){
            std::string serialised = server_macaroon_.serialize();
            for(size_t i = 0; i < serialised.size(); i++) {
                mb_mapping->tab_string[i] = (uint8_t)serialised[i];
            }
        } else {
            memset(mb_mapping->tab_string, 0, MODBUS_MAX_STRING_LENGTH * sizeof(uint8_t));
        }
    } else {
        /**
         * process_macaroon() needs an address range, which is tricky
         * for write_and_read_registers, since it has two ranges
         *
         * we need to find the entire range that the function is trying to access
         * since there's no way to handle disjoint caveats
         *
         * based on modbus.c, addr = write_addr, nb = write_nb, addr_wr = read_addr, nb_wr = read_nb
         * */
        if(*function == MODBUS_FC_WRITE_AND_READ_REGISTERS){
            uint16_t write_addr = *addr;
            uint16_t read_addr = *addr_wr;
            int write_nb = *nb;
            int read_nb = *nb_wr;
            uint16_t write_addr_max = find_max_address(MODBUS_FC_WRITE_AND_READ_REGISTERS, write_addr, write_nb);
            uint16_t read_addr_max = find_max_address(MODBUS_FC_WRITE_AND_READ_REGISTERS, read_addr, read_nb);
            *addr = (write_addr < read_addr) ? write_addr : read_addr;
            *nb = ((write_addr < read_addr) ? (read_addr_max - write_addr) : (write_addr_max - read_addr)) / 2;
        }

        /**
         * Extract the previously-received Macaroon
         * If verification is fails, return -1
         * If verification passes, continue to process the request
         * */
        if(!process_macaroon(mb_mapping->tab_string, *function, *addr, *nb)) {
            return -1;
        }
    }

    std::cout << std::endl;
    print_modbus_decompose_request(ctx, req, offset, slave_id, function, addr, nb, addr_wr, nb_wr);
    std::cout << std::endl;
    print_mb_mapping(mb_mapping);

    /**
     * Set state to MACAROONS_X (completed work within macaroons_shim)
     * Return to cheri_macaroons_shim to call libmodbus:modbus_process_request()
     * */
    shim_state = MACAROONS_X;
    return modbus_process_request(ctx, req, req_length, rsp, rsp_length, mb_mapping,
                                    shim_type, shim_state);
}

/*
DANGER:  This came from GitHub...
https://github.com/InversePalindrome/Blog/tree/master/RandomString
*/
std::string
generate_key(std::size_t length)
{
    const std::string characters = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<> distribution(0, characters.size() - 1);

    std::string random_string;

    for (std::size_t i = 0; i < length; ++i)
    {
        random_string += characters[distribution(generator)];
    }

    return random_string;
}

