#include "sfts.h"

#include <stdlib.h>

#include "kvdbo.h"

#include "kvunicode.h"
#include "kvserialization.h"
#include "kvassert.h"

#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

static void db_put(sfts * index, std::string & key, std::string & value);
static int db_get(sfts * index, std::string & key, std::string * p_value);
static void db_delete(sfts * index, std::string & key);
static int db_flush(sfts * index);
static int tokenize(sfts * index, uint64_t doc, const UChar * text);
static int add_to_indexer(sfts * index, uint64_t doc, const char * word,
                          std::set<uint64_t> & wordsids_set);
static void start_implicit_transaction_if_needed(sfts * index);

// . -> next word id
// ,[docid] -> [words ids]
// /[word id] -> word
// word -> [word id], [docs ids]

struct sfts {
    kvdbo * sfts_db;
    bool sfts_opened;
    // stored values, pending in transaction in memory.
    std::unordered_map<std::string, std::string> sfts_buffer;
    // identifiers of the dirty buffers.
    std::unordered_set<std::string> sfts_buffer_dirty;
    // identifiers of the deleted buffers.
    std::unordered_set<std::string> sfts_deleted;
    // whether a transaction has been opened.
    bool in_transaction;
    // if a transaction is opened, whether it's an implicit transaction.
    bool implicit_transaction;
    // number of pending changes in the transaction.
    unsigned int implicit_transaction_op_count;
};

sfts * sfts_new(const char * filename)
{
    sfts * result = new sfts;
    result->sfts_db = kvdbo_new(filename);
    result->sfts_opened = false;
    result->in_transaction = false;
    result->implicit_transaction = false;
    result->implicit_transaction_op_count = 0;
    return result;
}

void sfts_free(sfts * index)
{
    if (index->sfts_opened) {
        fprintf(stderr, "sfts: %s should be closed before freeing\n", sfts_get_filename(index));
    }
    kvdbo_free(index->sfts_db);
    free(index);
}

const char * sfts_get_filename(sfts * index)
{
    return kvdbo_get_filename(index->sfts_db);
}

int sfts_open(sfts * index)
{
    int r;
    
    if (index->sfts_opened) {
        fprintf(stderr, "sfts: %s already opened\n", sfts_get_filename(index));
        return KVDB_ERROR_NONE;
    }
    
    r = kvdbo_open(index->sfts_db);
    if (r < 0) {
        return r;
    }
    
    index->sfts_opened = true;
    
    return KVDB_ERROR_NONE;
}

int sfts_close(sfts * index)
{
    int r;
    
    if (!index->sfts_opened) {
        fprintf(stderr, "sfts: %s not opened\n", sfts_get_filename(index));
        return KVDB_ERROR_NONE;
    }
    
    if (index->in_transaction) {
        if (!index->implicit_transaction) {
            fprintf(stderr, "sfts: transaction not closed properly.\n");
        }
        r = sfts_transaction_commit(index);
        if (r < 0) {
            return r;
        }
    }
    kvdbo_close(index->sfts_db);
    index->sfts_opened = false;
    
    return KVDB_ERROR_NONE;
}

//int sfts_set(lidx * index, uint64_t doc, const char * text);
// text -> wordboundaries -> transliterated word -> store word with new word id
// word -> append doc id to docs ids
// store doc id -> words ids

int sfts_set(sfts * index, uint64_t doc, const char * text)
{
    UChar * utext = kv_from_utf8(text);
    int r = sfts_u_set(index, doc, utext);
    free(utext);
    return r;
}

int sfts_set2(sfts * index, uint64_t doc, const char ** text, int count)
{
    UChar ** utext = (UChar **) malloc(count * sizeof(* utext));
    for(int i = 0 ; i < count ; i ++) {
        utext[i] = kv_from_utf8(text[i]);
    }
    int result = sfts_u_set2(index, doc, (const UChar **) utext, count);
    for(int i = 0 ; i < count ; i ++) {
        free((void *) utext[i]);
    }
    free((void *) utext);
    return result;
}

int sfts_u_set(sfts * index, uint64_t doc, const UChar * utext)
{
    start_implicit_transaction_if_needed(index);
    int r = sfts_remove(index, doc);
    if (r == KVDB_ERROR_NOT_FOUND) {
        // do nothing.
    }
    else if (r < 0) {
        return r;
    }
    r = tokenize(index, doc, utext);
    if (r < 0) {
        return r;
    }
    index->implicit_transaction_op_count ++;
    return KVDB_ERROR_NONE;
}

int sfts_u_set2(sfts * index, uint64_t doc, const UChar ** utext, int count)
{
    start_implicit_transaction_if_needed(index);
    int r = sfts_remove(index, doc);
    if (r == KVDB_ERROR_NOT_FOUND) {
        // do nothing.
    }
    else if (r < 0) {
        return r;
    }
    int result = KVDB_ERROR_NONE;
    std::set<uint64_t> wordsids_set;
    for(unsigned int i = 0 ; i < count ; i ++) {
        char * transliterated = kv_transliterate(utext[i], kv_u_get_length(utext[i]));
        if (transliterated == NULL) {
            continue;
        }
        int r = add_to_indexer(index, doc, transliterated, wordsids_set);
        if (r < 0) {
            result = r;
            break;
        }
        free(transliterated);
    }
    if (result < 0) {
        return result;
    }
    
    std::string key(",");
    kv_encode_uint64(key, doc);
    
    std::string value_str;
    for(std::set<uint64_t>::iterator wordsids_set_iterator = wordsids_set.begin() ; wordsids_set_iterator != wordsids_set.end() ; ++ wordsids_set_iterator) {
        kv_encode_uint64(value_str, * wordsids_set_iterator);
    }
    db_put(index, key, value_str);
    index->implicit_transaction_op_count ++;
    
    return KVDB_ERROR_NONE;
}

static int tokenize(sfts * index, uint64_t doc, const UChar * text)
{
    int result = KVDB_ERROR_NONE;
    std::set<uint64_t> wordsids_set;
#if __APPLE__
    unsigned int len = kv_u_get_length(text);
    CFStringRef str = CFStringCreateWithBytes(NULL, (const UInt8 *) text, len * sizeof(* text), kCFStringEncodingUTF16LE, false);
    CFStringTokenizerRef tokenizer = CFStringTokenizerCreate(NULL, str, CFRangeMake(0, len), kCFStringTokenizerUnitWord, NULL);
    while (1) {
        CFStringTokenizerTokenType wordKind = CFStringTokenizerAdvanceToNextToken(tokenizer);
        if (wordKind == kCFStringTokenizerTokenNone) {
            break;
        }
        if (wordKind == kCFStringTokenizerTokenHasNonLettersMask) {
            continue;
        }
        CFRange range = CFStringTokenizerGetCurrentTokenRange(tokenizer);
        char * transliterated = kv_transliterate(&text[range.location], (int) range.length);
        if (transliterated == NULL) {
            continue;
        }
        int r = add_to_indexer(index, doc, transliterated, wordsids_set);
        if (r < 0) {
            result = r;
            break;
        }
        
        free(transliterated);
    }
    CFRelease(str);
    CFRelease(tokenizer);
#else
    UErrorCode status;
    status = U_ZERO_ERROR;
    UBreakIterator * iterator = ubrk_open(UBRK_WORD, NULL, text, u_strlen(text), &status);
    LIDX_ASSERT(status <= U_ZERO_ERROR);
    
    int32_t left = 0;
    int32_t right = 0;
    int word_kind = 0;
    ubrk_first(iterator);
    
    while (1) {
        left = right;
        right = ubrk_next(iterator);
        if (right == UBRK_DONE) {
            break;
        }
        
        word_kind = ubrk_getRuleStatus(iterator);
        if (word_kind == 0) {
            // skip punctuation and space.
            continue;
        }
        
        char * transliterated = lidx_transliterate(&text[left], right - left);
        if (transliterated == NULL) {
            continue;
        }
        int r = add_to_indexer(index, doc, transliterated, wordsids_set);
        if (r < 0) {
            result = r;
            break;
        }
        
        free(transliterated);
    }
    ubrk_close(iterator);
#endif
    if (result != 0) {
        return result;
    }
    
    std::string key(",");
    kv_encode_uint64(key, doc);
    
    std::string value_str;
    for(std::set<uint64_t>::iterator wordsids_set_iterator = wordsids_set.begin() ; wordsids_set_iterator != wordsids_set.end() ; ++ wordsids_set_iterator) {
        kv_encode_uint64(value_str, * wordsids_set_iterator);
    }
    db_put(index, key, value_str);
    
    return KVDB_ERROR_NONE;
}

static int add_to_indexer(sfts * index, uint64_t doc, const char * word,
                          std::set<uint64_t> & wordsids_set)
{
    std::string word_str(word);
    std::string value;
    uint64_t wordid;
    
    //fprintf(stderr, "adding word: %s\n", word);
    
    int r = db_get(index, word_str, &value);
    if (r == KVDB_ERROR_NONE) {
        // Adding doc id to existing entry.
        kv_decode_uint64(value, 0, &wordid);
        kv_encode_uint64(value, doc);
        db_put(index, word_str, value);
    }
    else if (r == KVDB_ERROR_NOT_FOUND) {
        // Not found.
        
        // Creating an entry.
        // store word with new id
        
        // read next word it
        std::string str;
        std::string nextwordidkey(".");
        int r = db_get(index, nextwordidkey, &str);
        if (r == KVDB_ERROR_NOT_FOUND) {
            // normal situation, create the nextwordid key to store the
            // current value.
            wordid = 0;
        }
        else if (r < 0) {
            return r;
        }
        else {
            kv_decode_uint64(str, 0, &wordid);
        }
        
        // write next word id
        std::string value;
        uint64_t next_wordid = wordid;
        next_wordid ++;
        kv_encode_uint64(value, next_wordid);
        db_put(index, nextwordidkey, value);
        
        std::string value_str;
        kv_encode_uint64(value_str, wordid);
        kv_encode_uint64(value_str, doc);
        db_put(index, word_str, value_str);
        
        std::string key("/");
        kv_encode_uint64(key, wordid);
        db_put(index, key, word_str);
    }
    else {
        return r;
    }
    
    wordsids_set.insert(wordid);
    
    return KVDB_ERROR_NONE;
}

//int sfts_remove(lidx * index, uint64_t doc);
// docid -> words ids -> remove docid from word
// if docs ids for word is empty, we remove the word id

static int get_word_for_wordid(sfts * index, uint64_t wordid, std::string & result);
static int remove_docid_in_word(sfts * index, std::string word, uint64_t doc);
static void remove_word(sfts * index, std::string word, uint64_t wordid);

int sfts_remove(sfts * index, uint64_t doc)
{
    std::string key(",");
    kv_encode_uint64(key, doc);
    std::string str;
    int r = db_get(index, key, &str);
    if (r == KVDB_ERROR_NOT_FOUND) {
        return KVDB_ERROR_NOT_FOUND;
    }
    else if (r < 0) {
        return r;
    }
    
    size_t position = 0;
    while (position < str.size()) {
        uint64_t wordid;
        position = kv_decode_uint64(str, position, &wordid);
        std::string word;
        r = get_word_for_wordid(index, wordid, word);
        if (r == KVDB_ERROR_NOT_FOUND) {
            return KVDB_ERROR_CORRUPTED;
        }
        else if (r < 0) {
            return r;
        }
        if (word.size() == 0) {
            continue;
        }
        int r = remove_docid_in_word(index, word, doc);
        if (r < 0) {
            return r;
        }
    }
    
    return KVDB_ERROR_NONE;
}

static int get_word_for_wordid(sfts * index, uint64_t wordid, std::string & result)
{
    std::string wordidkey("/");
    kv_encode_uint64(wordidkey, wordid);
    int r = db_get(index, wordidkey, &result);
    if (r < 0) {
        return r;
    }
    return KVDB_ERROR_NONE;
}

static int remove_docid_in_word(sfts * index, std::string word, uint64_t doc)
{
    std::string str;
    int r = db_get(index, word, &str);
    if (r == KVDB_ERROR_NOT_FOUND) {
        return KVDB_ERROR_NONE;
    }
    else if (r < 0) {
        return r;
    }
    
    uint64_t wordid;
    std::string buffer;
    size_t position = 0;
    position = kv_decode_uint64(str, position, &wordid);
    while (position < str.size()) {
        uint64_t current_docid;
        position = kv_decode_uint64(str, position, &current_docid);
        if (current_docid != doc) {
            kv_encode_uint64(buffer, current_docid);
        }
    }
    if (buffer.size() == 0) {
        // remove word entry
        remove_word(index, word, wordid);
    }
    else {
        // update word entry
        db_put(index, word, buffer);
    }
    
    return KVDB_ERROR_NONE;
}

static void remove_word(sfts * index, std::string word, uint64_t wordid)
{
    std::string wordidkey("/");
    kv_encode_uint64(wordidkey, wordid);
    db_delete(index, wordidkey);
    db_delete(index, word);
}

//int sfts_search(lidx * index, const char * token);
// token -> transliterated token -> docs ids

int sfts_search(sfts * index, const char * token, sfts_search_kind kind, uint64_t ** p_docsids, size_t * p_count)
{
    int result;
    UChar * utoken = kv_from_utf8(token);
    result = sfts_u_search(index, utoken, kind, p_docsids, p_count);
    free((void *) utoken);
    return result;
}

int sfts_u_search(sfts * index, const UChar * utoken, sfts_search_kind kind,
                  uint64_t ** p_docsids, size_t * p_count)
{
    int r;
    r = db_flush(index);
    if (r < 0) {
        return r;
    }
    
    char * transliterated = kv_transliterate(utoken, -1);
    unsigned int transliterated_length = (unsigned int) strlen(transliterated);
    std::set<uint64_t> result_set;
    
    kvdbo_iterator * iterator = kvdbo_iterator_new(index->sfts_db);
    if (kind == sfts_search_kind_prefix) {
        r = kvdbo_iterator_seek_after(iterator, transliterated, strlen(transliterated));
    }
    else {
        r = kvdbo_iterator_seek_first(iterator);
    }
    if (r < 0) {
        kvdbo_iterator_free(iterator);
        return r;
    }
    while (kvdbo_iterator_is_valid(iterator)) {
        bool add_to_result = false;
        
        const char * key;
        size_t key_size;
        kvdbo_iterator_get_key(iterator, &key, &key_size);
        std::string key_str(key, key_size);
        if (key_str.find(".") == 0 || key_str.find(",") == 0 || key_str.find("/") == 0) {
            r = kvdbo_iterator_next(iterator);
            if (r < 0) {
                kvdbo_iterator_free(iterator);
                return r;
            }
            continue;
        }
        if (kind == sfts_search_kind_prefix) {
            if (key_str.find(transliterated) != 0) {
                break;
            }
            add_to_result = true;
        }
        else if (kind == sfts_search_kind_substr) {
            //fprintf(stderr, "matching: %s %s\n", key_str.c_str(), transliterated);
            if (key_str.find(transliterated) != std::string::npos) {
                add_to_result = true;
            }
        }
        else if (kind == sfts_search_kind_suffix) {
            if ((key_str.length() >= transliterated_length) &&
                (key_str.compare(key_str.length() - transliterated_length, transliterated_length, transliterated) == 0)) {
                add_to_result = true;
            }
        }
        if (add_to_result) {
            size_t position = 0;
            uint64_t wordid;
            char * value;
            size_t value_size;
            int r = kvdbo_get(index->sfts_db, key_str.c_str(), key_str.length(), &value, &value_size);
            if (r == KVDB_ERROR_NOT_FOUND) {
                fprintf(stderr, "sfts probably corrupted: value not found for key %s\n", key_str.c_str());
                kvdbo_iterator_free(iterator);
                return KVDB_ERROR_CORRUPTED;
            }
            else if (r < 0) {
                kvdbo_iterator_free(iterator);
                return r;
            }
            std::string value_str(value, value_size);
            free(value);
            position = kv_decode_uint64(value_str, position, &wordid);
            while (position < value_str.size()) {
                uint64_t docid;
                position = kv_decode_uint64(value_str, position, &docid);
                result_set.insert(docid);
            }
        }
        
        r = kvdbo_iterator_next(iterator);
        if (r < 0) {
            return r;
        }
    }
    kvdbo_iterator_free(iterator);
    
    free(transliterated);
    
    uint64_t * result = (uint64_t *) calloc(result_set.size(), sizeof(* result));
    unsigned int count = 0;
    for(std::set<uint64_t>::iterator set_iterator = result_set.begin() ; set_iterator != result_set.end() ; ++ set_iterator) {
        result[count] = * set_iterator;
        count ++;
    }
    
    * p_docsids = result;
    * p_count = count;
    
    return KVDB_ERROR_NONE;
}

static void db_put(sfts * index, std::string & key, std::string & value)
{
    index->sfts_deleted.erase(key);
    index->sfts_buffer[key] = value;
    index->sfts_buffer_dirty.insert(key);
}

static int db_get(sfts * index, std::string & key, std::string * p_value)
{
    if (index->sfts_deleted.find(key) != index->sfts_deleted.end()) {
        return KVDB_ERROR_NOT_FOUND;
    }
    
    if (index->sfts_buffer.find(key) != index->sfts_buffer.end()) {
        * p_value = index->sfts_buffer[key];
        return KVDB_ERROR_NONE;
    }
    
    char * value;
    size_t value_size;
    int r = kvdbo_get(index->sfts_db, key.c_str(), key.length(), &value, &value_size);
    if (r < 0) {
        return r;
    }
    * p_value = std::string(value, value_size);
    index->sfts_buffer[key] = * p_value;
    free(value);
    return KVDB_ERROR_NONE;
}

static void db_delete(sfts * index, std::string & key)
{
    index->sfts_deleted.insert(key);
    index->sfts_buffer_dirty.erase(key);
    index->sfts_buffer.erase(key);
}

static int db_flush(sfts * index)
{
    if ((index->sfts_buffer_dirty.size() == 0) && (index->sfts_deleted.size() == 0)) {
        return KVDB_ERROR_NONE;
    }
    
    int r;
    for(std::unordered_set<std::string>::iterator set_iterator = index->sfts_buffer_dirty.begin() ; set_iterator != index->sfts_buffer_dirty.end() ; ++ set_iterator) {
        std::string key = * set_iterator;
        std::string value = index->sfts_buffer[key];
        r = kvdbo_set(index->sfts_db, key.c_str(), key.length(), value.c_str(), value.length());
        if (r < 0) {
            return r;
        }
    }
    for(std::unordered_set<std::string>::iterator set_iterator = index->sfts_deleted.begin() ; set_iterator != index->sfts_deleted.end() ; ++ set_iterator) {
        std::string key = * set_iterator;
        r = kvdbo_delete(index->sfts_db, key.c_str(), key.length());
        if (r == KVDB_ERROR_NOT_FOUND) {
            // do nothing.
        }
        else if (r < 0) {
            return r;
        }
    }
    index->sfts_buffer.clear();
    index->sfts_buffer_dirty.clear();
    index->sfts_deleted.clear();
    return KVDB_ERROR_NONE;
}

void sfts_transaction_begin(sfts * index)
{
    index->in_transaction = true;
    kvdbo_transaction_begin(index->sfts_db);
}

void sfts_transaction_abort(sfts * index)
{
    index->sfts_buffer.clear();
    index->sfts_buffer_dirty.clear();
    index->sfts_deleted.clear();
    kvdbo_transaction_abort(index->sfts_db);
    index->in_transaction = false;
    index->implicit_transaction = false;
}

int sfts_transaction_commit(sfts * index)
{
    if ((index->sfts_buffer.size() == 0) && (index->sfts_buffer_dirty.size() == 0) &&
        (index->sfts_deleted.size() == 0)) {
        sfts_transaction_abort(index);
        return KVDB_ERROR_NONE;
    }
    
    index->in_transaction = false;
    index->implicit_transaction = false;
    int r = db_flush(index);
    if (r < 0) {
        kvdbo_transaction_abort(index->sfts_db);
        return r;
    }
    r = kvdbo_transaction_commit(index->sfts_db);
    if (r < 0) {
        return r;
    }
    return KVDB_ERROR_NONE;
}

#define IMPLICIT_TRANSACTION_MAX_OP 100

static void start_implicit_transaction_if_needed(sfts * index)
{
    if (index->implicit_transaction && (index->implicit_transaction_op_count > IMPLICIT_TRANSACTION_MAX_OP)) {
        sfts_transaction_commit(index);
    }
    
    if (index->in_transaction) {
        return;
    }
    
    index->implicit_transaction = true;
    index->implicit_transaction_op_count = 0;
    sfts_transaction_begin(index);
}
