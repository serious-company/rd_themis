
/*
* Copyright (c) 2016 Cossack Labs Limited
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "redismodule.h"

#include <pthread.h>
#include <string.h>
#include <themis/themis.h>

static int scell_encrypt(RedisModuleCtx *ctx, RedisModuleString *key_name, const uint8_t* pass, size_t pass_len, const uint8_t* message, size_t message_len){
  size_t encrypted_data_len=0;
  if(THEMIS_BUFFER_TOO_SMALL!=themis_secure_cell_encrypt_seal(pass, pass_len, NULL, 0, message, message_len, NULL, &encrypted_data_len)){
    return -1;      
  }
  RedisModuleKey *key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_WRITE);
  if(REDISMODULE_OK != RedisModule_StringTruncate(key, encrypted_data_len)){
    RedisModule_DeleteKey(key);
    RedisModule_CloseKey(key);
    return -1;
  }
  uint8_t* encrypted_data = (uint8_t*)(RedisModule_StringDMA(key, &encrypted_data_len, REDISMODULE_WRITE));
  themis_status_t res = themis_secure_cell_encrypt_seal((const uint8_t*)pass, pass_len, NULL, 0, (const uint8_t*)message, message_len, encrypted_data, &encrypted_data_len);
  if(THEMIS_SUCCESS!=res){
    RedisModule_DeleteKey(key);
    RedisModule_CloseKey(key);
    return -1;
  }
  RedisModule_CloseKey(key);
  return 0;
}

static int scell_decrypt(RedisModuleCtx *ctx, RedisModuleString *key_name, const uint8_t* pass, size_t pass_len, uint8_t** decrypted_data, size_t* decrypted_data_len){
    RedisModuleKey *key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_READ);
    if(NULL == key){
      return -2;
    }

    if (REDISMODULE_KEYTYPE_STRING != RedisModule_KeyType(key)) {
      RedisModule_CloseKey(key);
      return -3;
    }

    size_t message_len=0;
    const uint8_t *message=(const uint8_t*)(RedisModule_StringDMA(key, &message_len, REDISMODULE_READ));
    if(THEMIS_BUFFER_TOO_SMALL!=themis_secure_cell_decrypt_seal(pass, pass_len, NULL, 0, message, message_len, NULL, decrypted_data_len)){
      RedisModule_CloseKey(key);
      return -1;
    }
    *decrypted_data = malloc(*decrypted_data_len);
    if(!(*decrypted_data)){
      RedisModule_CloseKey(key);
      return -1;
    }
    if(THEMIS_SUCCESS!=themis_secure_cell_decrypt_seal(pass, pass_len, NULL, 0, message, message_len, *decrypted_data, decrypted_data_len)){
      RedisModule_CloseKey(key);      
      free(*decrypted_data);
      return -1;
    }
    RedisModule_CloseKey(key);
    return 0;
}

static int cmd_scell_seal_encrypt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    size_t pass_len=0, message_len=0;
    const uint8_t *pass = (const uint8_t*)RedisModule_StringPtrLen(argv[2], &pass_len);
    const uint8_t *message = (const uint8_t*)RedisModule_StringPtrLen(argv[3], &message_len);
    if(0 != scell_encrypt(ctx, argv[1], pass, pass_len, message, message_len)){
      RedisModule_ReplyWithError(ctx, "ERR secure seal encryption failed");
      return REDISMODULE_ERR;      
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

static int cmd_scell_seal_decrypt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t decrypted_data_len=0, pass_len=0;
    const uint8_t *pass = (const uint8_t*)(RedisModule_StringPtrLen(argv[2], &pass_len));
    uint8_t* decrypted_data=NULL;
    int res = scell_decrypt(ctx, argv[1], pass, pass_len, &decrypted_data, &decrypted_data_len);
    switch(res){
    case -2:
      RedisModule_ReplyWithLongLong(ctx, 0);
      return REDISMODULE_OK;
    case -3:
      RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
      return REDISMODULE_ERR;
    case 0:
      RedisModule_ReplyWithStringBuffer(ctx, (const char*)decrypted_data, decrypted_data_len);
      free(decrypted_data);
      return REDISMODULE_OK;
    }
      RedisModule_ReplyWithError(ctx, "ERR secure seal decryption failed");
      return REDISMODULE_ERR;      
}

int scell_enc_reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  long res = (long)RedisModule_GetBlockedClientPrivateData(ctx);
  switch(res){
  case 0:
    return RedisModule_ReplyWithSimpleString(ctx, "OK");    
  }
  return RedisModule_ReplyWithError(ctx, "ERR secure seal encryption failed");
}

int scell_enc_timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

void* scell_enc_thread(void* arg){
  void **targ = arg;
  RedisModuleCtx *ctx = targ[0];
  RedisModuleBlockedClient *bc = targ[1];
  RedisModuleString **argv = (RedisModuleString **)targ[2];
  RedisModule_Free(targ);
  size_t pass_len=0, message_len=0;
  const uint8_t *pass = (const uint8_t*)RedisModule_StringPtrLen(argv[2], &pass_len);
  const uint8_t *message = (const uint8_t*)RedisModule_StringPtrLen(argv[3], &message_len);
  long res = scell_encrypt(ctx, argv[1], pass, pass_len, message, message_len);
  RedisModule_UnblockClient(bc,(void*)res);
  return NULL;
}

static int cmd_scell_seal_encrypt_block(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, scell_enc_reply, scell_enc_timeout, NULL, 2000);
    void **targ = RedisModule_Alloc(sizeof(void*)*4);
    targ[0] = ctx;
    targ[1] = bc;
    targ[2] = (void*)argv;
    if (pthread_create(&tid,NULL,scell_enc_thread,(void*)targ) != 0) {
      RedisModule_AbortBlock(bc);
      return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;    
}

int scell_dec_reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  void** res = RedisModule_GetBlockedClientPrivateData(ctx);
  switch((long)(res[0])){
  case -2:
    return RedisModule_ReplyWithLongLong(ctx, 0);
  case -3:
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  case 0:
    return RedisModule_ReplyWithStringBuffer(ctx, (const char*)res[1], (size_t)res[2]);
  }
  return RedisModule_ReplyWithError(ctx, "ERR secure seal decryption failed");
}

int scell_dec_timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

void scell_dec_free(void *privdata) {
  RedisModule_Free(privdata);
}
void* scell_dec_thread(void* arg){
  void **targ = arg;
  RedisModuleCtx *ctx = targ[0];
  RedisModuleBlockedClient *bc = targ[1];
  RedisModuleString **argv = (RedisModuleString **)targ[2];
  RedisModule_Free(targ);
  size_t pass_len=0;
  const uint8_t *pass = (const uint8_t*)(RedisModule_StringPtrLen(argv[2], &pass_len));
  void **res = RedisModule_Alloc(3*sizeof(void*));
  long a = scell_decrypt(ctx, argv[1], pass, pass_len, (uint8_t**)(&(res[1])), (size_t*)(&(res[2])));
  res[0] = (void*)a;
  RedisModule_UnblockClient(bc,(void*)res);
  return NULL;
}

static int cmd_scell_seal_decrypt_block(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, scell_dec_reply, scell_dec_timeout, scell_dec_free, 2000);
    void **targ = RedisModule_Alloc(sizeof(void*)*4);
    targ[0] = ctx;
    targ[1] = bc;
    targ[2] = (void*)argv;
    if (pthread_create(&tid,NULL,scell_dec_thread,(void*)targ) != 0) {
      RedisModule_AbortBlock(bc);
      return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;    
}


static size_t smessage_encrypt_len(const uint8_t* data, const uint32_t data_length, const uint8_t* private_key, const uint32_t private_key_length, const uint8_t* public_key, const uint32_t public_key_length, const uint8_t* peer_public_key, const uint32_t peer_public_key_length){
  size_t enc_len=0;
  if(THEMIS_BUFFER_TOO_SMALL!=themis_secure_message_wrap(private_key, private_key_length, peer_public_key, peer_public_key_length, data, data_length, NULL, &enc_len)){
    return 0;
  }
  return enc_len+sizeof(public_key_length)+public_key_length;
}

//encrypt data with acra ctruct 
static int smessage_encrypt(const uint8_t* data, const uint32_t data_length, const uint8_t* private_key, const uint32_t private_key_length, const uint8_t* public_key, const uint32_t public_key_length, const uint8_t* peer_public_key, const uint32_t peer_public_key_length, uint8_t* enc_data, uint32_t *enc_data_length){
  if(*enc_data_length<sizeof(public_key_length)+public_key_length){
    return -2;
  }
  memcpy(enc_data, &public_key_length, sizeof(public_key_length));
  memcpy(enc_data+sizeof(public_key_length), public_key, public_key_length);
  size_t enc_len=*enc_data_length-sizeof(public_key_length)-public_key_length;
  if(THEMIS_SUCCESS!=themis_secure_message_wrap(private_key, private_key_length, peer_public_key, peer_public_key_length, data, data_length, enc_data+sizeof(public_key_length)+public_key_length, &enc_len)){
    return -2;
  }
  *enc_data_length = enc_len+sizeof(public_key_length)+public_key_length;
  return 0;
}

//decrypt  acra structed data 
static int smessage_decrypt(const uint8_t* data, const uint32_t data_length, const uint8_t* private_key, const uint32_t private_key_length, uint8_t** dec_data, uint32_t* dec_data_length){
  const uint32_t public_key_length = *((const uint32_t*)data);
  if(data_length<=public_key_length+sizeof(uint32_t)){
    return -1;
  }
  const uint8_t* public_key = data+sizeof(uint32_t);
  const uint8_t* data_ = public_key+public_key_length;
  size_t data_length_ = data_length-public_key_length-sizeof(uint32_t); 
  
  if(THEMIS_BUFFER_TOO_SMALL!=themis_secure_message_unwrap(private_key, private_key_length, public_key, public_key_length, data_, data_length_, NULL, (size_t*)dec_data_length)){
    return -1;
  }
  *dec_data = malloc(*dec_data_length);
  if(!(*dec_data)){
    return -2;
  }
  if(THEMIS_SUCCESS!=themis_secure_message_unwrap(private_key, private_key_length, public_key, public_key_length, data_, data_length_, *dec_data, (size_t*)dec_data_length)){
    free(*dec_data);
    return -1;
  }
  return 0;
}

static int smessage_dec(const uint8_t* private_key, const uint32_t private_key_length, const uint8_t* data, const uint8_t data_length, uint8_t** dec_data, uint32_t* dec_data_length){
  return smessage_decrypt(data, data_length, private_key, private_key_length, dec_data, dec_data_length);
}

static int smessage_e(RedisModuleCtx *ctx, RedisModuleString *key_name, const uint8_t* public_key, size_t public_key_len, const uint8_t* message, size_t message_len){
    uint8_t new_private_key[10240];
    uint8_t new_public_key[10240];
    size_t new_private_key_length=10240, new_public_key_length=10240;
    if(THEMIS_SUCCESS!=themis_gen_ec_key_pair(new_private_key, &new_private_key_length, new_public_key, &new_public_key_length)){
      return -1;      
    }
    size_t encrypted_data_len = smessage_encrypt_len((const uint8_t*)message, message_len, new_private_key, new_private_key_length,  new_public_key, new_public_key_length, (const uint8_t*)public_key, public_key_len);
    if(0==encrypted_data_len){
      return -1;            
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_WRITE);
    if(REDISMODULE_OK != RedisModule_StringTruncate(key, encrypted_data_len)){
      RedisModule_DeleteKey(key);
      RedisModule_CloseKey(key);
      return -1;            
    }
    uint8_t* encrypted_data = (uint8_t*)(RedisModule_StringDMA(key, &encrypted_data_len, REDISMODULE_WRITE));

    if(0 != smessage_encrypt((const uint8_t*)message, message_len, new_private_key, new_private_key_length, new_public_key, new_public_key_length, (const uint8_t*)public_key, public_key_len, encrypted_data, (uint32_t*)(&encrypted_data_len))){
      RedisModule_DeleteKey(key);
      RedisModule_CloseKey(key);
      return -1;      
    }
    RedisModule_CloseKey(key);
    return 0;  
}

static int cmd_smessage_encrypt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    size_t public_key_len=0, message_len=0;
    const uint8_t *public_key = (const uint8_t*)RedisModule_StringPtrLen(argv[2], &public_key_len);
    const uint8_t *message = (const uint8_t*)RedisModule_StringPtrLen(argv[3], &message_len);
    int res = smessage_e(ctx, argv[1], public_key, public_key_len, message, message_len);
    switch(res){
    case 0:
      RedisModule_ReplyWithSimpleString(ctx, "OK");
      return REDISMODULE_OK;      
    }
    RedisModule_ReplyWithError(ctx, "ERR secure message encryption failed");
    return REDISMODULE_ERR;      
}

int smessage_enc_reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  long res = (long)RedisModule_GetBlockedClientPrivateData(ctx);
  switch(res){
  case 0:
    return RedisModule_ReplyWithSimpleString(ctx, "OK");    
  }
  return RedisModule_ReplyWithError(ctx, "ERR secure seal encryption failed");
}

int smessage_enc_timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

void* smessage_enc_thread(void* arg){
  void **targ = arg;
  RedisModuleCtx *ctx = targ[0];
  RedisModuleBlockedClient *bc = targ[1];
  RedisModuleString **argv = (RedisModuleString **)targ[2];
  RedisModule_Free(targ);
  size_t pass_len=0, message_len=0;
  const uint8_t *pass = (const uint8_t*)RedisModule_StringPtrLen(argv[2], &pass_len);
  const uint8_t *message = (const uint8_t*)RedisModule_StringPtrLen(argv[3], &message_len);
  long res = smessage_e(ctx, argv[1], pass, pass_len, message, message_len);
  RedisModule_UnblockClient(bc,(void*)res);
  return NULL;
}

static int cmd_smessage_encrypt_block(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, smessage_enc_reply, smessage_enc_timeout, NULL, 2000);
    void **targ = RedisModule_Alloc(sizeof(void*)*4);
    targ[0] = ctx;
    targ[1] = bc;
    targ[2] = (void*)argv;
    if (pthread_create(&tid,NULL,smessage_enc_thread,(void*)targ) != 0) {
      RedisModule_AbortBlock(bc);
      return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;    
}

static int smessage_d(RedisModuleCtx *ctx, RedisModuleString *key_name, const uint8_t* private_key, size_t private_key_len, uint8_t** decrypted_data, size_t* decrypted_data_len){
    RedisModuleKey *key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_READ);
    if(NULL == key){
      return -2;
    }

    if (REDISMODULE_KEYTYPE_STRING != RedisModule_KeyType(key)) {
      RedisModule_CloseKey(key);
      return -3;
    }

    size_t message_len=0;
    const uint8_t *message=(const uint8_t*)(RedisModule_StringDMA(key, &message_len, REDISMODULE_READ));
    if(0 != smessage_dec(private_key, private_key_len, message, message_len, decrypted_data, (uint32_t*)decrypted_data_len)){
      RedisModule_CloseKey(key);
      return -1;
    }
    RedisModule_CloseKey(key);
    return 0;
}

static int cmd_smessage_decrypt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t decrypted_data_len=0, private_key_len=0;
    const uint8_t *private_key = (const uint8_t*)(RedisModule_StringPtrLen(argv[2], &private_key_len));
    uint8_t *decrypted_data = NULL;
    int res = smessage_d(ctx, argv[1], private_key, private_key_len,&decrypted_data, &decrypted_data_len);
    switch(res){
    case -2:
      RedisModule_ReplyWithLongLong(ctx, 0);
      return REDISMODULE_OK;
    case -3:
      RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
      return REDISMODULE_ERR;
    case 0:
      RedisModule_ReplyWithStringBuffer(ctx, (const char*)decrypted_data, decrypted_data_len);
      free(decrypted_data);
      return REDISMODULE_OK;
    }
    RedisModule_ReplyWithError(ctx, "ERR secure message decryption failed");
    return REDISMODULE_ERR;      
}

int smessage_dec_reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  void** res = RedisModule_GetBlockedClientPrivateData(ctx);
  switch((long)(res[0])){
  case -2:
    return RedisModule_ReplyWithLongLong(ctx, 0);
  case -3:
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  case 0:
    return RedisModule_ReplyWithStringBuffer(ctx, (const char*)res[1], (size_t)res[2]);
  }
  return RedisModule_ReplyWithError(ctx, "ERR secure message decryption failed");
}

int smessage_dec_timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

void smessage_dec_free(void *privdata) {
  RedisModule_Free(privdata);
}
void* smessage_dec_thread(void* arg){
  void **targ = arg;
  RedisModuleCtx *ctx = targ[0];
  RedisModuleBlockedClient *bc = targ[1];
  RedisModuleString **argv = (RedisModuleString **)targ[2];
  RedisModule_Free(targ);
  size_t pass_len=0;
  const uint8_t *pass = (const uint8_t*)(RedisModule_StringPtrLen(argv[2], &pass_len));
  void **res = RedisModule_Alloc(3*sizeof(void*));
  long a = smessage_d(ctx, argv[1], pass, pass_len, (uint8_t**)(&(res[1])), (size_t*)(&(res[2])));
  res[0] = (void*)a;
  RedisModule_UnblockClient(bc,(void*)res);
  return NULL;
}

static int cmd_smessage_decrypt_block(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
  }

  pthread_t tid;
  RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, smessage_dec_reply, smessage_dec_timeout, smessage_dec_free, 2000);
  void **targ = RedisModule_Alloc(sizeof(void*)*4);
  targ[0] = ctx;
  targ[1] = bc;
  targ[2] = (void*)argv;
  if (pthread_create(&tid,NULL,smessage_dec_thread,(void*)targ) != 0) {
    RedisModule_AbortBlock(bc);
    return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
  }
  return REDISMODULE_OK;    
}


int RedisModule_OnLoad(RedisModuleCtx *ctx) {
    if (RedisModule_Init(ctx, "rd_themis", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.cset", cmd_scell_seal_encrypt, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.cget", cmd_scell_seal_decrypt, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.csetbl", cmd_scell_seal_encrypt_block, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.cgetbl", cmd_scell_seal_decrypt_block, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.msset", cmd_smessage_encrypt, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.msget", cmd_smessage_decrypt, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.mssetbl", cmd_smessage_encrypt_block, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "rd_themis.msgetbl", cmd_smessage_decrypt_block, "no-monitor fast", 1, 1, 1) == REDISMODULE_ERR)
      return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
