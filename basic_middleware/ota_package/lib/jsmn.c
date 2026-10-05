#include <stddef.h>

#include "jsmn.h"

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser,
                                   jsmntok_t *tokens,
                                   size_t num_tokens)
{
    jsmntok_t *tok;

    if (parser->toknext >= num_tokens) {
        return NULL;
    }

    tok = &tokens[parser->toknext++];
    tok->start = -1;
    tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    tok->type = JSMN_UNDEFINED;
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token,
                            jsmntype_t type,
                            int start,
                            int end)
{
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser,
                                const char *js,
                                size_t len,
                                jsmntok_t *tokens,
                                size_t num_tokens)
{
    int start = (int)parser->pos;
    jsmntok_t *token;

    for (; parser->pos < len; parser->pos++) {
        switch (js[parser->pos]) {
            case '\t':
            case '\r':
            case '\n':
            case ' ':
            case ',':
            case ']':
            case '}':
                goto found;
            default:
                if ((unsigned char)js[parser->pos] < 32U ||
                    js[parser->pos] == '\"') {
                    parser->pos = (unsigned int)start;
                    return JSMN_ERROR_INVAL;
                }
                break;
        }
    }

found:
    token = jsmn_alloc_token(parser, tokens, num_tokens);
    if (token == NULL) {
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
    }

    jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
    token->parent = parser->toksuper;
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser,
                             const char *js,
                             size_t len,
                             jsmntok_t *tokens,
                             size_t num_tokens)
{
    int start = (int)parser->pos;
    jsmntok_t *token;

    parser->pos++;

    for (; parser->pos < len; parser->pos++) {
        char ch = js[parser->pos];

        if (ch == '\"') {
            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (token == NULL) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
            }
            jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
            token->parent = parser->toksuper;
            return 0;
        }

        if (ch == '\\') {
            parser->pos++;
            if (parser->pos >= len) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_PART;
            }

            switch (js[parser->pos]) {
                case '\"':
                case '/':
                case '\\':
                case 'b':
                case 'f':
                case 'r':
                case 'n':
                case 't':
                    break;
                case 'u': {
                    int i;
                    for (i = 0; i < 4; ++i) {
                        parser->pos++;
                        if (parser->pos >= len) {
                            parser->pos = (unsigned int)start;
                            return JSMN_ERROR_PART;
                        }
                        if (!((js[parser->pos] >= '0' && js[parser->pos] <= '9') ||
                              (js[parser->pos] >= 'A' && js[parser->pos] <= 'F') ||
                              (js[parser->pos] >= 'a' && js[parser->pos] <= 'f'))) {
                            parser->pos = (unsigned int)start;
                            return JSMN_ERROR_INVAL;
                        }
                    }
                    break;
                }
                default:
                    parser->pos = (unsigned int)start;
                    return JSMN_ERROR_INVAL;
            }
        }
    }

    parser->pos = (unsigned int)start;
    return JSMN_ERROR_PART;
}

void jsmn_init(jsmn_parser *parser)
{
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

int jsmn_parse(jsmn_parser *parser, const char *js, unsigned int len,
               jsmntok_t *tokens, unsigned int num_tokens)
{
    int count;
    int i;

    for (; parser->pos < len; parser->pos++) {
        char ch = js[parser->pos];
        jsmntok_t *token;
        int ret;

        switch (ch) {
            case '{':
            case '[':
                count = (int)parser->toknext;
                token = jsmn_alloc_token(parser, tokens, num_tokens);
                if (token == NULL) {
                    return JSMN_ERROR_NOMEM;
                }
                if (parser->toksuper != -1) {
                    tokens[parser->toksuper].size++;
                    token->parent = parser->toksuper;
                }
                token->type = (ch == '{') ? JSMN_OBJECT : JSMN_ARRAY;
                token->start = (int)parser->pos;
                parser->toksuper = count;
                break;
            case '}':
            case ']':
                for (i = (int)parser->toknext - 1; i >= 0; i--) {
                    token = &tokens[i];
                    if (token->start != -1 && token->end == -1) {
                        if ((token->type == JSMN_OBJECT && ch == '}') ||
                            (token->type == JSMN_ARRAY && ch == ']')) {
                            token->end = (int)parser->pos + 1;
                            parser->toksuper = token->parent;
                            break;
                        }
                        return JSMN_ERROR_INVAL;
                    }
                }
                if (i == -1) {
                    return JSMN_ERROR_INVAL;
                }
                break;
            case '\"':
                ret = jsmn_parse_string(parser, js, len, tokens, num_tokens);
                if (ret < 0) {
                    return ret;
                }
                if (parser->toksuper != -1) {
                    tokens[parser->toksuper].size++;
                }
                break;
            case '\t':
            case '\r':
            case '\n':
            case ' ':
                break;
            case ':':
                parser->toksuper = (int)parser->toknext - 1;
                break;
            case ',':
                if (parser->toksuper != -1 &&
                    tokens[parser->toksuper].type != JSMN_ARRAY &&
                    tokens[parser->toksuper].type != JSMN_OBJECT) {
                    parser->toksuper = tokens[parser->toksuper].parent;
                }
                break;
            default:
                ret = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
                if (ret < 0) {
                    return ret;
                }
                if (parser->toksuper != -1) {
                    tokens[parser->toksuper].size++;
                }
                break;
        }
    }

    for (i = (int)parser->toknext - 1; i >= 0; i--) {
        if (tokens[i].start != -1 && tokens[i].end == -1) {
            return JSMN_ERROR_PART;
        }
    }

    return (int)parser->toknext;
}
