#ifndef ROSE_SATPROBLEM_H
#define ROSE_SATPROBLEM_H

#include <boost/array.hpp>
#include <boost/lexical_cast.hpp>
#include <vector>
#include <set>

typedef int Var;
typedef int Lit; // Using Dimacs conventions, but with 0 for false and 0x80000000 for true
#define LitList(N) boost::array<Lit, (N)> // Stored LSB first, counted in bits
typedef std::vector<Lit> Clause;
// Len and similar template parameters are always in bits, not bytes
const Lit TRUE = 0x80000000, FALSE = 0;
inline bool isConstantBool(Lit lit) {return (lit & 0x7fffffff) == 0;}
inline int invert(Lit lit) {if (isConstantBool(lit)) return lit ^ 0x80000000; else return -lit;}

template <size_t Len>
inline LitList(Len) invertWord(const LitList(Len)& a) {
  LitList(Len) result;
  for (size_t i = 0; i < Len; ++i) result[i] = invert(a[i]);
  return result;
}
inline Lit invertMaybe(Lit a, bool inv) {
  return inv ? invert(a) : a;
}

inline LitList(1) single(Lit a) {
  LitList(1) result;
  result[0] = a;
  return result;
}

template <size_t From, size_t To, size_t InputSize>
LitList(To - From) extract(const LitList(InputSize)& in) {
  LitList(To - From) result;
  for (size_t i = 0; i < To - From; ++i) {
    result[i] = in[i + From];
  }
  return result;
}

template <size_t Len1, size_t Len2>
LitList(Len1 + Len2) concat(const LitList(Len1)& a, const LitList(Len2)& b) {
  LitList(Len1 + Len2) result;
  for (size_t i = 0; i < Len1; ++i) result[i] = a[i];
  for (size_t i = 0; i < Len2; ++i) result[i + Len1] = b[i];
  return result;
}

template <size_t NumBits>
LitList(NumBits) number(unsigned int n) {
  LitList(NumBits) result;
  for (size_t i = 0; i < NumBits; ++i) {
    result[i] = (n % 2 == 1) ? TRUE : FALSE;
    n >>= 1;
  }
  return result;
}

static bool absLess(Lit a, Lit b) {
  if (abs(a) < abs(b)) return true;
  if (abs(a) > abs(b)) return false;
  return a < b;
}

static bool clauseLess(const Clause& a, const Clause& b) {
  Clause absa(a), absb(b);
  for (size_t i = 0; i < absa.size(); ++i) absa[i] = abs(absa[i]);
  for (size_t i = 0; i < absb.size(); ++i) absb[i] = abs(absb[i]);
  if (absa < absb) return true;
  if (absb < absa) return false;
  return a < b;
}

template <size_t Len>
static std::vector<Lit> toVector(const LitList(Len)& lits) {
  return std::vector<Lit>(lits.begin(), lits.end());
}

static std::vector<Lit> toVector(Lit l) {
  return std::vector<Lit>(1, l);
}

struct UnsatisfiableException {};

struct SatProblem {
  int numVariables;
  std::set<Clause, bool(*)(const Clause&, const Clause&)> clauses;
  std::map<std::vector<Lit>, Lit> andGateCSE;
  std::map<LitList(3), Lit> muxCSE; // Fields are sel, ifTrue, ifFalse
  std::vector<std::pair<std::string, std::vector<Lit> > > interfaceLiterals;
  FILE* outfile;

  public:
  SatProblem(FILE* f): numVariables(0), clauses(clauseLess), outfile(f) {}

  Var newVar() {return ++numVariables;}

  template <size_t Count>
  LitList(Count) newVars() {LitList(Count) vl; for (size_t i = 0; i < Count; ++i) vl[i] = newVar(); return vl;}

  void addInterface(const std::string& name, const std::vector<Lit>& values) {
    interfaceLiterals.push_back(std::make_pair(name, values));
  }

  void addClause(const Clause& cl) {
    Clause newCl;
    for (size_t i = 0; i < cl.size(); ++i) {
      if (cl[i] == FALSE) {
      } else if (cl[i] == TRUE) {
        return;
      } else {
        newCl.push_back(cl[i]);
      }
    }
    if (newCl.empty()) {throw UnsatisfiableException();}
    std::sort(newCl.begin(), newCl.end(), absLess);
    newCl.erase(std::unique(newCl.begin(), newCl.end()), newCl.end());
    for (size_t i = 0; i < cl.size(); ++i) {
      for (size_t j = 0; j < i; ++j) {
        if (newCl[i] == -newCl[j]) {
          return;
        }
      }
    }
    clauses.insert(newCl);
  }

  template <size_t Len>
  void addClause(const LitList(Len)& cl) {
    addClause(toVector(cl));
  }

  void toDimacs() const {
    for (size_t i = 0; i < interfaceLiterals.size(); ++i) {
      fprintf(outfile, "c %s %zu", interfaceLiterals[i].first.c_str(), interfaceLiterals[i].second.size());
      for (size_t j = 0; j < interfaceLiterals[i].second.size(); ++j) {
        fprintf(outfile, " %d", interfaceLiterals[i].second[j]);
      }
      fputc('\n', outfile);
    }
    fprintf(outfile, "p cnf %d %zu\n", numVariables, clauses.size());
    toDimacsWithoutHeader();
  }

  void toDimacsWithoutHeader() const {
    for (std::set<Clause, bool(*)(const Clause&, const Clause&)>::const_iterator i = clauses.begin(); i != clauses.end(); ++i) {
      const Clause& cl = *i;
      for (size_t j = 0; j < cl.size(); ++j) {
        fprintf(outfile, "%d ", cl[j]);
      }
      fputs("0\n", outfile);
    }
  }

  void condEquivalence(Lit sel, Lit a, Lit b) {
    LitList(3) cl;
    cl[0] = invert(sel);
    cl[1] = invert(a);
    cl[2] = b;
    addClause(cl);
    cl[1] = a;
    cl[2] = invert(b);
    addClause(cl);
  }

  Lit mux(Lit sel, Lit ifTrue, Lit ifFalse) {
    if (sel == FALSE) {
      return ifFalse;
    } else if (sel == TRUE) {
      return ifTrue;
    } else if (ifTrue == ifFalse) {
      return ifTrue;
    } else if (ifTrue == TRUE && ifFalse == FALSE) {
      return sel;
    } else if (ifTrue == FALSE && ifFalse == TRUE) {
      return invert(sel);
    } else {
      if (sel < 0) {
        sel = invert(sel);
        std::swap(ifTrue, ifFalse);
      }
      LitList(3) cseLookup;
      cseLookup[0] = sel;
      cseLookup[1] = ifTrue;
      cseLookup[2] = ifFalse;
      std::map<LitList(3), Lit>::const_iterator it = muxCSE.find(cseLookup);
      if (it != muxCSE.end()) return it->second;
      Lit output = newVar();
      condEquivalence(sel, ifTrue, output);
      condEquivalence(invert(sel), ifFalse, output);
      muxCSE.insert(std::make_pair(cseLookup, output));
      return output;
    }
  }

  template <size_t Len>
  Lit andAcross(const LitList(Len)& a) {
    std::vector<Lit> newA;
    for (size_t i = 0; i < Len; ++i) {
      if (a[i] == FALSE) {
        return FALSE;
      } else if (a[i] == TRUE) {
      } else {
        newA.push_back(a[i]);
      }
    }
    if (newA.size() == 1) {
      return newA[0];
    }
    if (newA.empty()) return TRUE;
    std::sort(newA.begin(), newA.end(), absLess);
    std::map<std::vector<Lit>, Lit>::const_iterator it = andGateCSE.find(newA);
    if (it != andGateCSE.end()) return it->second;
    Lit output = newVar();
    for (size_t i = 0; i < newA.size(); ++i) {
      LitList(2) cl1;
      cl1[0] = invert(output);
      cl1[1] = newA[i];
      addClause(cl1);
    }
    std::vector<Lit> cl2;
    for (size_t i = 0; i < newA.size(); ++i) {
      cl2.push_back(invert(newA[i]));
    }
    cl2.push_back(output);
    addClause(cl2);
    andGateCSE.insert(std::make_pair(newA, output));
    return output;
  }

  Lit andGate(Lit a, Lit b) {
    LitList(2) inputs;
    inputs[0] = a;
    inputs[1] = b;
    return andAcross(inputs);
  }

  Lit nandGate(Lit a, Lit b) {
    return invert(andGate(a, b));
  }

  Lit orGate(Lit a, Lit b) {
    return invert(andGate(invert(a), invert(b)));
  }

  template <size_t Len>
  Lit orAcross(const LitList(Len)& a) {
    LitList(Len) aInv = invertWord(a);
    return invert(andAcross(aInv));
  }

  Lit norGate(Lit a, Lit b) {
    return invert(orGate(a, b));
  }

  template <size_t Len>
  Lit norAcross(const LitList(Len)& a) {
    return invert(orAcross(a));
  }

  Lit xorGate(Lit a, Lit b) {
    return mux(a, invert(b), b);
  }

  template <size_t Len>
  Lit xorAcross(const LitList(Len)& a) {
    Lit result = (Len == 0 ? FALSE : a[0]);
    for (size_t i = 1; i < Len; ++i) result = xorGate(result, a[i]);
    return result;
  }

  template <size_t Len>
  LitList(Len) invertWord(const LitList(Len)& a) {
    LitList(Len) result;
    for (size_t i = 0; i < Len; ++i) result[i] = invert(a[i]);
    return result;
  }

  template <size_t Len>
  LitList(Len) andWords(const LitList(Len)& a, const LitList(Len)& b) {
    LitList(Len) output;
    for (size_t i = 0; i < Len; ++i) {
      output[i] = andGate(a[i], b[i]);
    }
    return output;
  }

  template <size_t Len>
  LitList(Len) orWords(const LitList(Len)& a, const LitList(Len)& b) {
    LitList(Len) output;
    for (size_t i = 0; i < Len; ++i) {
      output[i] = orGate(a[i], b[i]);
    }
    return output;
  }

  template <size_t Len>
  LitList(Len) xorWords(const LitList(Len)& a, const LitList(Len)& b) {
    LitList(Len) output;
    for (size_t i = 0; i < Len; ++i) {
      output[i] = xorGate(a[i], b[i]);
    }
    return output;
  }

  template <size_t NBits>
  Lit equal(const LitList(NBits)& a, const LitList(NBits)& b) {
    return norAcross(xorWords(a, b));
  }

  template <size_t NBits>
  LitList(NBits) ite(Lit cond, const LitList(NBits)& ifTrue, const LitList(NBits)& ifFalse) {
    LitList(NBits) output;
    for (size_t i = 0; i < NBits; ++i) {
      output[i] = mux(cond, ifTrue[i], ifFalse[i]);
    }
    return output;
  }

  template <size_t NBits>
  LitList(NBits) adder(const LitList(NBits)& a, const LitList(NBits)& b, Lit carryIn = FALSE, LitList(NBits)* carriesOutOpt = NULL) {
    LitList(NBits) result;
    for (size_t i = 0; i < NBits; ++i) {
      Lit sumOut = xorGate(xorGate(a[i], b[i]), carryIn);
      Lit carryOut = orGate(andGate(a[i], orGate(b[i], carryIn)), andGate(b[i], carryIn));
      result[i] = sumOut;
      if (carriesOutOpt != NULL) {
        (*carriesOutOpt)[i] = carryOut;
      }
      carryIn = carryOut;
    }
    return result;
  }

  template <size_t Len1, size_t Len2>
  LitList(Len1 + Len2) signedMultiplier(const LitList(Len1)& a, const LitList(Len2)& b) {
    return newVars<Len1 + Len2>(); // FIXME
  }

  template <size_t Len1, size_t Len2>
  LitList(Len1 + Len2) unsignedMultiplier(const LitList(Len1)& a, const LitList(Len2)& b) {
    return newVars<Len1 + Len2>(); // FIXME
  }

  template <size_t Len1, size_t Len2>
  LitList(Len1 + Len2) signedDivider(const LitList(Len1)& dividend, const LitList(Len2)& divisor) {
    // Returns Len1 bits quotient, Len2 bits remainder concatenated
    return newVars<Len1 + Len2>(); // FIXME
  }

  template <size_t Len1, size_t Len2>
  LitList(Len1 + Len2) unsignedDivider(const LitList(Len1)& dividend, const LitList(Len2)& divisor) {
    // Returns Len1 bits quotient, Len2 bits remainder concatenated
    return newVars<Len1 + Len2>(); // FIXME
  }

  template <size_t Len, size_t SCLen>
  LitList(Len) rightShifter(const LitList(Len)& in, const LitList(SCLen)& shiftCount) {
    // Logarithmic shifter
    LitList(Len) result = in;
    for (size_t scBit = SCLen, pow2scBit = 1 << (SCLen - 1); scBit > 0; --scBit, pow2scBit >>= 1) {
      LitList(Len) resultCopy = result;
      for (size_t i = 0; i < Len; ++i) {
        resultCopy[i] = mux(shiftCount[scBit - 1], (i >= pow2scBit ? result[i - pow2scBit] : FALSE), result[i]);
      }
      result = resultCopy;
    }
    return result;
  }

  template <size_t Len, size_t SCLen>
  LitList(Len) leftShifter(const LitList(Len)& in, const LitList(SCLen)& shiftCount) {
    // Logarithmic shifter
    LitList(Len) result = in;
    for (size_t scBit = SCLen, pow2scBit = 1 << (SCLen - 1); scBit > 0; --scBit, pow2scBit >>= 1) {
      LitList(Len) resultCopy = result;
      for (size_t i = 0; i < Len; ++i) {
        resultCopy[i] = mux(shiftCount[scBit - 1], (i + pow2scBit >= Len ? FALSE : result[i + pow2scBit]), result[i]);
      }
      result = resultCopy;
    }
    return result;
  }

  template <size_t Len, size_t SCLen>
  LitList(Len) arithmeticLeftShifter(const LitList(Len)& in, const LitList(SCLen)& shiftCount) {
    // Logarithmic shifter
    LitList(Len) result = in;
    for (size_t scBit = SCLen, pow2scBit = 1 << (SCLen - 1); scBit > 0; --scBit, pow2scBit >>= 1) {
      LitList(Len) resultCopy = result;
      for (size_t i = 0; i < Len; ++i) {
        resultCopy[i] = mux(shiftCount[scBit - 1], (i + pow2scBit >= Len ? result[Len - 1] : result[i + pow2scBit]), result[i]);
      }
      result = resultCopy;
    }
    return result;
  }

  template <size_t Len, size_t SCLen>
  LitList(Len) rightRotater(const LitList(Len)& in, const LitList(SCLen)& shiftCount) {
    // Logarithmic shifter
    LitList(Len) result = in;
    for (size_t scBit = SCLen, pow2scBit = 1 << (SCLen - 1); scBit > 0; --scBit, pow2scBit >>= 1) {
      LitList(Len) resultCopy = result;
      for (size_t i = 0; i < Len; ++i) {
        resultCopy[i] = mux(shiftCount[scBit - 1], result[(i - pow2scBit + Len) % Len], result[i]);
      }
      result = resultCopy;
    }
    return result;
  }

  template <size_t Len, size_t SCLen>
  LitList(Len) leftRotater(const LitList(Len)& in, const LitList(SCLen)& shiftCount) {
    // Logarithmic shifter
    LitList(Len) result = in;
    for (size_t scBit = SCLen, pow2scBit = 1 << (SCLen - 1); scBit > 0; --scBit, pow2scBit >>= 1) {
      LitList(Len) resultCopy = result;
      for (size_t i = 0; i < Len; ++i) {
        resultCopy[i] = mux(shiftCount[scBit - 1], result[(i + pow2scBit) % Len], result[i]);
      }
      result = resultCopy;
    }
    return result;
  }

  template <size_t Len>
  LitList(Len) bitReverse(const LitList(Len)& in) {
    LitList(Len) result;
    for (size_t i = Len; i > 0; --i) {
      result[i - 1] = in[Len - i];
    }
    return result;
  }

  template <size_t Len>
  LitList(Len) leastSignificantSetBit(const LitList(Len)& in) {
    LitList(Len) result = number<Len>(0);
    for (size_t i = Len; i > 0; --i) {
      result = ite(in[i - 1], number<Len>(i - 1), result);
    }
    return result;
  }

};

#endif // ROSE_SATPROBLEM_H
