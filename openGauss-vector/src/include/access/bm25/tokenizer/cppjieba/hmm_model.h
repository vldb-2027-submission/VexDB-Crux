/**
 * Copyright ...
 */

#ifndef CPPJIEBA_HMMMODEL_H
#define CPPJIEBA_HMMMODEL_H

#include <vtl/hashtable>

#include "access/bm25/tokenizer/cppjieba/string_util.h"
#include "access/bm25/tokenizer/cppjieba/unicode.h"
#include "utils/palloc.h"

namespace cppjieba {
#define READLINESIZE 1024 * 1024 * 50
using EmitProbMap = UnorderedMap<Rune, double>;

class HMMModel : public BaseObject {
public:
    /*
     * STATUS:
     * 0: HMMModel::B, 1: HMMModel::E, 2: HMMModel::M, 3:HMMModel::S
     * */
    enum { B = 0, E = 1, M = 2, S = 3, STATUS_SUM = 4 };

    HMMModel(const char *modelPath)
    {
        memset(startProb, 0, sizeof(startProb));
        memset(transProb, 0, sizeof(transProb));
        statMap[0] = 'B';
        statMap[1] = 'E';
        statMap[2] = 'M';
        statMap[3] = 'S';
        LoadModel(modelPath);
    }
    void destroy() 
    {
        for(size_t i = 0; i < STATUS_SUM; ++i) {
            ann_helper::optional_destroy(emitProbVec[i]);
        }
    }

    void LoadModel(const char *filePath)
    {
        std::ifstream ifile(filePath);
        Assert(ifile.is_open());

        char *tokens[STATUS_SUM] = {NULL};
        // Load startProb
        char *line = (char *)palloc(READLINESIZE);
        char *l = GetLine(ifile, line);
        [[maybe_unused]] /* keep compiler happy */
        size_t tokens_num = split(tokens, l, " ", STATUS_SUM);
        Assert(tokens_num == STATUS_SUM);
        for (size_t j = 0; j < STATUS_SUM; ++j) {
            startProb[j] = atof(tokens[j]);
        }

        // Load transProb
        for (size_t i = 0; i < STATUS_SUM; ++i) {
            l = GetLine(ifile, line);
            [[maybe_unused]] /* keep compiler happy */
            size_t tokens_num = split(tokens, l, " ", STATUS_SUM);
            Assert(tokens_num == STATUS_SUM);
            for (size_t j = 0; j < STATUS_SUM; ++j) {
                transProb[i][j] = atof(tokens[j]);
            }
        }

        // Load emitProbB
        l = GetLine(ifile, line);
        LoadEmitProb(l, emitProbVec[0]);

        // Load emitProbE
        l = GetLine(ifile, line);
        LoadEmitProb(l, emitProbVec[1]);

        // Load emitProbM
        l = GetLine(ifile, line);
        LoadEmitProb(l, emitProbVec[2]);

        // Load emitProbS
        l = GetLine(ifile, line);
        LoadEmitProb(l, emitProbVec[3]);

        pfree(line);
    }

    double GetEmitProb(const EmitProbMap &ptMp, Rune key, double defVal) const
    {
        auto cit = ptMp.cfind(key);
        if (cit == ptMp.cend()) {
            return defVal;
        }
        return cit->second;
    }

    char *GetLine(std::ifstream &ifile, char *line)
    {
        char *res = NULL;
        while (ifile.getline(line, READLINESIZE)) {
            res = ascii_trim(line);
            if (strlen(res) == 0) {
                continue;
            }
            if (res[0] == '#') {
                continue;
            }
            return res;
        }
        return res;
    }

    void LoadEmitProb(char *line, EmitProbMap &mp)
    {
        if (strlen(line) == 0) {
            return;
        }
        Unicode unicode;
        char *token = NULL;
        char *save_ptr = NULL;
        token = strtok_s(line, ",", &save_ptr);

        constexpr size_t emit_args_num = 2ul;
        char *single_word[emit_args_num] = {NULL};
        while (token != NULL) {
            [[maybe_unused]] /* keep compiler happy */
            size_t args_num = split(single_word, token, ":", emit_args_num);
            Assert(args_num == emit_args_num);

            if(!DecodeRunesInString(single_word[0], unicode) || unicode.size() != 1) {
                ereport(ERROR,
                    (errcode(ERRCODE_FILE_READ_FAILED),
                     errmsg("illegal line: \"%s\"", line)));
                return;
            }
            token = strtok_s(NULL, ",", &save_ptr);
            mp[unicode[0]] = atof(single_word[1]);
        }
    }

    char statMap[STATUS_SUM];
    double startProb[STATUS_SUM];
    double transProb[STATUS_SUM][STATUS_SUM];
    EmitProbMap emitProbVec[STATUS_SUM];
};

}  /* namespace cppjieba */

#endif /* CPPJIEBA_HMMMODEL_H */
