#include "jsd/jsd_sdo.h"

#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "jsd/jsd_print.h"

///////////////////  ASYNC SDO /////////////////////////////

void jsd_sdo_req_cirq_init(jsd_sdo_req_cirq_t* self, const char* name) {
  assert(self);
  self->r     = 0;
  self->w     = 0;
  self->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;

  strncpy(self->name, name, JSD_NAME_LEN);
}

// non-locking
static jsd_sdo_req_t queue_pop(jsd_sdo_req_cirq_t* self) {
  jsd_sdo_req_t new_req;

  // MSG_DEBUG("[%s] r=%u w=%u", self->name, self->r, self->w);
  new_req = self->buffer[self->r % JSD_SDO_REQ_CIRQ_LEN];
  self->r++;

  return new_req;
}

jsd_sdo_req_t jsd_sdo_req_cirq_pop(jsd_sdo_req_cirq_t* self) {
  assert(self);

  jsd_sdo_req_t new_req = {};

  if (jsd_sdo_req_cirq_is_empty(self)) {
    WARNING("Popping [%s] when empty", self->name);
    return new_req;
  }

  pthread_mutex_lock(&self->mutex);
  new_req = queue_pop(self);
  pthread_mutex_unlock(&self->mutex);

  return new_req;
}

// non-locking
static bool queue_push(jsd_sdo_req_cirq_t* self, jsd_sdo_req_t req) {
  bool status = true;

  self->buffer[self->w % JSD_SDO_REQ_CIRQ_LEN] = req;
  self->w++;
  if ((self->w - self->r) > JSD_SDO_REQ_CIRQ_LEN) {
    self->r++;
    WARNING("[%s] is overflowing: r=%u w=%u", self->name, self->r, self->w);
    status = false;
  }

  return status;
}

bool jsd_sdo_req_cirq_push(jsd_sdo_req_cirq_t* self, jsd_sdo_req_t req) {
  bool status;

  pthread_mutex_lock(&self->mutex);
  status = queue_push(self, req);
  pthread_mutex_unlock(&self->mutex);

  // SUCCESS("Pushed new message to [%s]", self->name);
  return status;
}

static bool queue_is_empty(jsd_sdo_req_cirq_t* self) {
  return self->r == self->w;
}

bool jsd_sdo_req_cirq_is_empty(jsd_sdo_req_cirq_t* self) {
  assert(self);
  bool val;
  pthread_mutex_lock(&self->mutex);
  val = queue_is_empty(self);
  pthread_mutex_unlock(&self->mutex);
  return val;
}

static void print_sdo_param(jsd_sdo_data_type_t data_type, uint16_t slave_id,
                            uint16_t index, uint8_t subindex, void* void_data,
                            char* verb) {
  jsd_sdo_data_t data = *(jsd_sdo_data_t*)void_data;

  switch (data_type) {
    case JSD_SDO_DATA_I8:
      MSG("Slave[%d] %s 0x%X:%d (I8)  = %d", slave_id, verb, index, subindex,
          data.as_i8);
      break;

    case JSD_SDO_DATA_I16:
      MSG("Slave[%d] %s 0x%X:%d (I16) = %d", slave_id, verb, index, subindex,
          data.as_i16);
      break;

    case JSD_SDO_DATA_I32:
      MSG("Slave[%d] %s 0x%X:%d (I32) = %d", slave_id, verb, index, subindex,
          data.as_i32);
      break;

    case JSD_SDO_DATA_I64:
      MSG("Slave[%d] %s 0x%X:%d (I64) = %ld", slave_id, verb, index, subindex,
          data.as_i64);
      break;

    case JSD_SDO_DATA_FLOAT:
      MSG("Slave[%d] %s 0x%X:%d (F32) = %f", slave_id, verb, index, subindex,
          data.as_float);
      break;

    case JSD_SDO_DATA_U8:
      MSG("Slave[%d] %s 0x%X:%d (U8)  = %u", slave_id, verb, index, subindex,
          data.as_u8);
      break;

    case JSD_SDO_DATA_U16:
      MSG("Slave[%d] %s 0x%X:%d (U16) = %u", slave_id, verb, index, subindex,
          data.as_u16);
      break;

    case JSD_SDO_DATA_U32:
      MSG("Slave[%d] %s 0x%X:%d (U32) = %u", slave_id, verb, index, subindex,
          data.as_u32);
      break;

    case JSD_SDO_DATA_U64:
      MSG("Slave[%d] %s 0x%X:%d (U64) = %lu", slave_id, verb, index, subindex,
          data.as_u64);
      break;

    default:
      WARNING("Slave[%d] data type unspecified", slave_id);
      break;
  }
}

void* sdo_thread_loop(void* void_data) {
  jsd_t* self = (jsd_t*)void_data;
  unsigned int handled_errors = 0;
  struct timespec ts;

  while (true) {
    pthread_mutex_lock(&self->jsd_sdo_req_cirq.mutex);

    while (queue_is_empty(&self->jsd_sdo_req_cirq)) {

      // wake up on async jsd conditional trigger for max responsiveness or at 1hz
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 1;
      pthread_cond_timedwait(&self->sdo_thread_cond, &self->jsd_sdo_req_cirq.mutex, &ts);

      if (self->sdo_join_flag) {
        pthread_mutex_unlock(&self->jsd_sdo_req_cirq.mutex);
        return NULL;
      }

      ec_mbxbuft MbxIn;
      int sid;
      for (sid = 1; sid <= *self->ecx_context.slavecount; sid++) {
        ecx_mbxreceive(&self->ecx_context, sid, &MbxIn, 0);
      }

      handled_errors = 0;
      while(ecx_iserror(&self->ecx_context) && (handled_errors++ < 50)) {
        ec_errort err;
        ecx_poperror(&self->ecx_context, &err);
        char* err_str = ecx_err2string(err);

        size_t len = strlen(err_str);
        if(err_str[len-1] == '\n'){
          err_str[len-1] = '\0';
        }

        ERROR("%s", err_str);
        // TODO push it onto a protected JSD elist... ugh
        // could adding another lock here result in deadlock?
      }
    }

    // pop off the request for application handling
    jsd_sdo_req_t req = queue_pop(&self->jsd_sdo_req_cirq);
    pthread_mutex_unlock(&self->jsd_sdo_req_cirq.mutex);

    int param_size = jsd_sdo_data_type_size(req.data_type);

    if (req.request_type == JSD_SDO_REQ_TYPE_WRITE) {
      req.wkc = ecx_SDOwrite(&self->ecx_context, req.slave_id, req.sdo_index,
                             req.sdo_subindex,
                             false,  // CA not used
                             param_size, (void*)&req.data, JSD_SDO_TIMEOUT);

      print_sdo_param(req.data_type, req.slave_id, req.sdo_index,
                      req.sdo_subindex, &req.data, "Write");

    } else {  // JSD_SDO_REQ_TYPE_READ

      req.wkc = ecx_SDOread(&self->ecx_context, req.slave_id, req.sdo_index,
                            req.sdo_subindex,
                            false,  // CA not used
                            &param_size, (void*)&req.data, JSD_SDO_TIMEOUT);

      print_sdo_param(req.data_type, req.slave_id, req.sdo_index,
                      req.sdo_subindex, &req.data, "Read");
    }

    req.success = (req.wkc == 1);

    // push to the response queue for application handling
    jsd_sdo_req_cirq_push(&self->jsd_sdo_res_cirq, req);
  }
}
//////////////////////////

int jsd_sdo_data_type_size(jsd_sdo_data_type_t type) {
  int size = 0;

  switch (type) {
    case JSD_SDO_DATA_I8:  // fallthrough intended
    case JSD_SDO_DATA_U8:
      size = 1;
      break;

    case JSD_SDO_DATA_I16:  // fallthrough intended
    case JSD_SDO_DATA_U16:
      size = 2;
      break;

    case JSD_SDO_DATA_I32:    // fallthrough intended
    case JSD_SDO_DATA_FLOAT:  // fallthrough intended
    case JSD_SDO_DATA_U32:
      size = 4;
      break;

    case JSD_SDO_DATA_I64:  // fallthrough intended
    case JSD_SDO_DATA_U64:
      size = 8;
      break;

    default:
      WARNING("Unknown jsd_sdo_data_type_t: %d", type);
  }
  return size;
}

jsd_sdo_req_t 
  jsd_sdo_populate_request(uint16_t slave_id, 
                           uint16_t index,
                           uint8_t subindex, 
                           jsd_sdo_data_type_t data_type,
                           void* data,
                           jsd_sdo_req_type_t request_type,
                           uint16_t app_id) 
{
  jsd_sdo_req_t req;

  req.slave_id     = slave_id;
  req.sdo_index    = index;
  req.sdo_subindex = subindex;
  req.data_type    = data_type;
  req.request_type = request_type;
  req.app_id       = app_id;

  if (request_type == JSD_SDO_REQ_TYPE_WRITE && data != NULL) {
    memcpy(&req.data, data, jsd_sdo_data_type_size(data_type));
  }else{
    memset(&req.data, 0, sizeof(req.data));
  }

  // setting these not strictly needed
  req.success = false;
  req.wkc = 0;

  return req;
}

void jsd_sdo_push_async_request(jsd_t* self, jsd_sdo_req_t request)
{
  jsd_sdo_req_cirq_push(&self->jsd_sdo_req_cirq, request);

  pthread_cond_signal(&self->sdo_thread_cond);

  jsd_slave_state_t* state = &self->slave_states[request.slave_id];

  state->num_async_sdo_requests++;
}

void jsd_sdo_set_param_async(jsd_t* self, uint16_t slave_id, uint16_t index,
                             uint8_t subindex, jsd_sdo_data_type_t data_type,
                             void* data, uint16_t app_id) 
{
  jsd_sdo_push_async_request(self, 
      jsd_sdo_populate_request(slave_id, index, subindex, data_type, data,
                             JSD_SDO_REQ_TYPE_WRITE, app_id));
}

void jsd_sdo_get_param_async(jsd_t* self, 
    uint16_t slave_id, uint16_t index,
                             uint8_t subindex, jsd_sdo_data_type_t data_type,
                             uint16_t app_id) 
{
  jsd_sdo_push_async_request(self, 
      jsd_sdo_populate_request(slave_id, index, subindex, data_type, NULL,
                             JSD_SDO_REQ_TYPE_READ, app_id));
}

///////////////////  BLOCKING SDO /////////////////////////////

bool jsd_sdo_set_param_blocking(ecx_contextt* ecx_context, uint16_t slave_id,
                                uint16_t index, uint8_t subindex,
                                jsd_sdo_data_type_t data_type, void* param_in) {
  assert(ecx_context);

  int param_size = jsd_sdo_data_type_size(data_type);

  int wkc = ecx_SDOwrite(ecx_context, slave_id, index, subindex, false,
                         param_size, param_in, JSD_SDO_TIMEOUT);
  if (wkc == 0) {
    MSG("Slave[%d] Failed to write SDO: 0x%X:%d", slave_id, index, subindex);
    return false;
  }

  print_sdo_param(data_type, slave_id, index, subindex, param_in, "Wrote");

  return true;
}

bool jsd_sdo_get_param_blocking(ecx_contextt* ecx_context, uint16_t slave_id,
                                uint16_t index, uint8_t subindex,
                                jsd_sdo_data_type_t data_type,
                                void*               param_out) {
  assert(ecx_context);

  int param_size = jsd_sdo_data_type_size(data_type);

  int wkc = ecx_SDOread(ecx_context, slave_id, index, subindex, false,
                        &param_size, param_out, JSD_SDO_TIMEOUT);
  if (wkc == 0) {
    MSG("Slave[%d] Failed to read SDO: 0x%X:%d", slave_id, index, subindex);
    return false;
  }

  print_sdo_param(data_type, slave_id, index, subindex, param_out, "Read");

  return true;
}

bool jsd_sdo_set_ca_param_blocking(ecx_contextt* ecx_context, uint16_t slave_id,
                                   uint16_t index, uint8_t subindex,
                                   int param_size, void* param_in) {
  assert(ecx_context);

  int wkc = ecx_SDOwrite(ecx_context, slave_id, index, subindex, true,
                         param_size, param_in, JSD_SDO_TIMEOUT);
  if (wkc == 0) {
    MSG("Slave[%d] Failed to write SDO: 0x%X:%d", slave_id, index, subindex);
    return false;
  }
  MSG("Slave[%d] Wrote 0x%X:%d register by Complete Access", slave_id, index,
      subindex);
  return true;
}

bool jsd_sdo_get_ca_param_blocking(ecx_contextt* ecx_context, uint16_t slave_id,
                                   uint16_t index, uint8_t subindex,
                                   int* param_size_in_out, void* param_out) {
  assert(ecx_context);

  int wkc = ecx_SDOread(ecx_context, slave_id, index, subindex, true,
                        param_size_in_out, param_out, JSD_SDO_TIMEOUT);
  if (wkc == 0) {
    MSG("Slave[%d] Failed to read SDO: 0x%X:%d", slave_id, index, subindex);
    return false;
  }
  MSG("Slave[%d] Read 0x%X:%d register by Complete Access", slave_id, index,
      subindex);
  return true;
}

void jsd_sdo_signal_emcy_check(jsd_t* self){
  self->raise_sdo_thread_cond = true;
}
