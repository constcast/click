// -*- related-file-name: "../include/click/lexer.hh" -*-
/*
 * lexer.{cc,hh} -- parses Click language files, produces Router objects
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2001-2003 International Computer Science Institute
 * Copyright (c) 2004-2011 Regents of the University of California
 * Copyright (c) 2008 Meraki, Inc.
 * Copyright (c) 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/lexer.hh>
#include <click/router.hh>
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
#include <click/variableenv.hh>
#include <click/bitvector.hh>
#include <click/standard/errorelement.hh>
#if CLICK_USERLEVEL
# include <click/userutils.hh>
#endif
CLICK_DECLS

#ifdef CLICK_LINUXMODULE
# define ADD_ELEMENT_TYPE(name, factory, thunk, scoped) \
		add_element_type((name), (factory), (thunk), 0, (scoped))
#else
# define ADD_ELEMENT_TYPE(name, factory, thunk, scoped) \
		add_element_type((name), (factory), (thunk), (scoped))
#endif

static const char * const port_names[2] = {"input", "output"};

static void
redeclaration_error(ErrorHandler *errh, const char *what, String name, const String &landmark, const String &old_landmark)
{
  if (!what)
    what = "";
  const char *sp = (strlen(what) ? " " : "");
  errh->lerror(landmark, "redeclaration of %s%s%<%s%>", what, sp, name.c_str());
  errh->lerror(old_landmark, "%<%s%> previously declared here", name.c_str());
}

//
// ELEMENT FACTORIES
//

static Element *
error_element_factory(uintptr_t)
{
  return new ErrorElement;
}

static Element *
compound_element_factory(uintptr_t)
{
  assert(0);
  return 0;
}

//
// CLASS LEXER::TUNNELEND
//

class Lexer::TunnelEnd {

  Router::Port _port;
  Vector<Router::Port> _correspond;
  int8_t _expanded;
  bool _isoutput;
  TunnelEnd *_other;
  TunnelEnd *_next;

  friend class Lexer;

 public:

  TunnelEnd(const Router::Port &port, bool isoutput, TunnelEnd *next)
    : _port(port), _expanded(0), _isoutput(isoutput), _other(0), _next(next) {
  }

  const Router::Port &port() const	{ return _port; }
  bool isoutput() const			{ return _isoutput; }
  TunnelEnd *next() const		{ return _next; }
  TunnelEnd *other() const		{ return _other; }

  void pair_with(TunnelEnd *d) {
    assert(!_other && !d->_other && _isoutput == !d->_isoutput
	   && _port.port == d->_port.port);
    _other = d;
    d->_other = this;
  }

  void expand(Lexer *, Vector<Router::Port> &);

};

//
// CLASS LEXER::COMPOUND
//

class Lexer::Compound : public Element { public:

  Compound(const String &, const String &, VariableEnvironment *parent);

  const String &name() const		{ return _name; }
  const char *printable_name_c_str();
  const String &landmark() const	{ return _landmark; }
  int nformals() const			{ return _nformals; }
  const VariableEnvironment &scope() const	{ return _scope; }
  VariableEnvironment &scope()		{ return _scope; }
  inline void define(const String &fname, const String &ftype, bool isformal, Lexer *);
  int depth() const			{ return _scope.depth(); }

    static String landmark_string(const String &filename, unsigned lineno) {
	if (!lineno)
	    return filename;
	else if (filename && (filename.back() == ':' || isspace((unsigned char) filename.back())))
	    return filename + String(lineno);
	else
	    return filename + ":" + String(lineno);
    }

    String element_landmark(int e) const {
	return landmark_string(_element_filenames[e], _element_linenos[e]);
    }

    int check_pseudoelement(int e, bool isoutput, const char *name, ErrorHandler *errh) const;
    void finish(Lexer *, ErrorHandler *);

  inline int assign_arguments(const Vector<String> &args, Vector<String> *values) const;
  int resolve(Lexer *, int etype, int ninputs, int noutputs, Vector<String> &, ErrorHandler *, const String &landmark);
  void expand_into(Lexer *, int, VariableEnvironment &);
  void connect(int from_idx, int from_port, int to_idx, int to_port);

  const char *class_name() const	{ return _name.c_str(); }
  void *cast(const char *);
  Compound *clone() const		{ return 0; }

  void set_overload_type(int t)		{ _overload_type = t; }
  inline Compound *overload_compound(Lexer *) const;

  String signature() const;
  static String signature(const String &name, const Vector<String> *formal_types, int nargs, int ninputs, int noutputs);

 private:

  mutable String _name;
  String _landmark;
  int _overload_type;

  VariableEnvironment _scope;
  int _nformals;
  int _ninputs;
  int _noutputs;
  bool _scope_order_error : 1;

  Vector<int> _elements;
  Vector<String> _element_names;
  Vector<String> _element_configurations;
  Vector<String> _element_filenames;
  Vector<unsigned> _element_linenos;
  Vector<int> _element_nports[2];
  Vector<Router::Connection> _conn;

  friend class Lexer;

};

Lexer::Compound::Compound(const String &name, const String &lm, VariableEnvironment *parent)
  : _name(name), _landmark(lm), _overload_type(-1),
    _scope(parent),
    _nformals(0), _ninputs(0), _noutputs(0), _scope_order_error(false)
{
}

const char *
Lexer::Compound::printable_name_c_str()
{
  if (_name)
    return _name.c_str();
  else
    return "<anonymous>";
}

void *
Lexer::Compound::cast(const char *s)
{
  if (strcmp(s, "Lexer::Compound") == 0 || _name == s)
    return this;
  else
    return 0;
}

inline void
Lexer::Compound::define(const String &name, const String &value, bool isformal, Lexer *l)
{
  assert(!isformal || _nformals == _scope.size());
  if (!_scope.define(name, value, false))
    l->lerror("parameter %<$%s%> multiply defined", name.c_str());
  else if (isformal) {
    _nformals = _scope.size();
    if (value)
      for (int i = 0; i < _scope.size() - 1; i++)
	if (_scope.value(i) == value) {
	  l->lerror("repeated keyword parameter %<%s%> in compound element", value.c_str());
	  break;
	}
    if (!_scope_order_error && _nformals > 1
	&& ((!value && _scope.value(_nformals - 2))
	    || _scope.value(_nformals - 2) == "__REST__")) {
      l->lerror("compound element parameters out of order\n(The correct order is %<[positional], [keywords], [__REST__]%>.)");
      _scope_order_error = true;
    }
  }
}

void
Lexer::Compound::connect(int from_idx, int from_port, int to_idx, int to_port)
{
  if (from_port < 0)
    from_port = 0;
  if (to_port < 0)
    to_port = 0;
  _conn.push_back(Router::Connection(from_idx, from_port, to_idx, to_port));
  if (_element_nports[0][to_idx] <= to_port)
    _element_nports[0][to_idx] = to_port + 1;
  if (_element_nports[1][from_idx] <= from_port)
    _element_nports[1][from_idx] = from_port + 1;
}

int
Lexer::Compound::check_pseudoelement(int which, bool isoutput, const char *name, ErrorHandler *errh) const
{
    Bitvector used(_element_nports[1-isoutput][which]);
    for (const Connection *it = _conn.begin(); it != _conn.end(); ++it)
	if ((*it)[1-isoutput].idx == which)
	    used[(*it)[1-isoutput].port] = true;
    if (_element_nports[isoutput][which])
	errh->error("%<%s%> pseudoelement %<%s%> may only be used as %s", name, port_names[isoutput], port_names[1-isoutput]);
    for (int i = 0; i < used.size(); i++)
	if (!used[i])
	    errh->error("%<%s%> %s %d missing", name, port_names[isoutput], i);
    return used.size();
}

void
Lexer::Compound::finish(Lexer *lexer, ErrorHandler *errh)
{
    assert(_element_names[0] == "input" && _element_names[1] == "output");
    LandmarkErrorHandler lerrh(errh, _landmark);
    _ninputs = check_pseudoelement(0, false, printable_name_c_str(), &lerrh);
    _noutputs = check_pseudoelement(1, true, printable_name_c_str(), &lerrh);

    // deanonymize element names
    for (int i = 0; i < _elements.size(); i++)
	if (_element_names[i][0] == ';')
	    _element_names[i] = lexer->deanonymize_element_name(_element_names[i], i);
}

inline Lexer::Compound *
Lexer::Compound::overload_compound(Lexer *lexer) const
{
  if (_overload_type >= 0 && lexer->_element_types[_overload_type].factory == compound_element_factory)
    return (Compound *) lexer->_element_types[_overload_type].thunk;
  else
    return 0;
}

inline int
Lexer::Compound::assign_arguments(const Vector<String> &args, Vector<String> *values) const
{
  return cp_assign_arguments(args, _scope.values().begin(), _scope.values().begin() + _nformals, values);
}

int
Lexer::Compound::resolve(Lexer *lexer, int etype, int ninputs, int noutputs, Vector<String> &args, ErrorHandler *errh, const String &landmark)
{
  // Try to return an element class, even if it is wrong -- the error messages
  // are friendlier
  Compound *ct = this;
  int closest_etype = -1;
  int nct = 0;

  while (ct) {
    nct++;
    if (ct->_ninputs == ninputs && ct->_noutputs == noutputs
	&& ct->assign_arguments(args, &args) >= 0)
      return etype;
    else if (ct->assign_arguments(args, 0) >= 0)
      closest_etype = etype;

    if (Compound *next = ct->overload_compound(lexer)) {
      etype = ct->_overload_type;
      ct = next;
    } else if (ct->_overload_type >= 0)
      return ct->_overload_type;
    else
      break;
  }

  if (nct != 1 || closest_etype < 0) {
    errh->lerror(landmark, "no match for %<%s%>", signature(name(), 0, args.size(), ninputs, noutputs).c_str());
    ContextErrorHandler cerrh(errh, "candidates are:");
    for (ct = this; ct; ct = ct->overload_compound(lexer))
      cerrh.lmessage(ct->landmark(), "%s", ct->signature().c_str());
  }
  ct = (closest_etype >= 0 ? (Compound *) lexer->_element_types[closest_etype].thunk : 0);
  if (ct)
    ct->assign_arguments(args, &args);
  return closest_etype;
}

String
Lexer::Compound::signature(const String &name, const Vector<String> *formal_types, int nargs, int ninputs, int noutputs)
{
  StringAccum sa;
  sa << (name ? name : String("<anonymous>"));

  if (formal_types && formal_types->size()) {
    sa << '(';
    for (int i = 0; i < formal_types->size(); i++) {
      if (i)
	sa << ", ";
      if ((*formal_types)[i] == "")
	sa << "<arg>";
      else if ((*formal_types)[i] == "__REST__")
	sa << "...";
      else
	sa << (*formal_types)[i];
    }
    sa << ')';
  }

  const char *pl_args = (nargs == 1 ? " argument, " : " arguments, ");
  const char *pl_ins = (ninputs == 1 ? " input, " : " inputs, ");
  const char *pl_outs = (noutputs == 1 ? " output" : " outputs");
  sa << '[';
  if (!formal_types && nargs > 0)
    sa << nargs << pl_args;
  sa << ninputs << pl_ins << noutputs << pl_outs;
  sa << ']';

  return sa.take_string();
}

String
Lexer::Compound::signature() const
{
  return signature(_name, &scope().values(), -1, _ninputs, _noutputs);
}

void
Lexer::Compound::expand_into(Lexer *lexer, int which, VariableEnvironment &ve)
{
    assert(_element_names[0] == "input" && _element_names[1] == "output");

    ErrorHandler *errh = lexer->_errh;
    String ename = lexer->_c->_element_names[which];

    lexer->_c->_elements[which] = TUNNEL_TYPE;
    int eidexes[3];
    lexer->add_tunnels(ename, eidexes);

    Vector<int> eidx_map;
    eidx_map.push_back(eidexes[1]);
    eidx_map.push_back(eidexes[2]);

    // 'name_slash' is 'name' constrained to end with a slash
    String ename_slash = ename + "/";
    for (int i = 2; i < _elements.size(); ++i) {
	String cname = ename_slash + _element_names[i];
	int eidx = lexer->_element_map[cname];
	if (eidx >= 0) {
	    redeclaration_error(errh, "element", cname, lexer->element_landmark(which), lexer->element_landmark(eidx));
	    eidx_map.push_back(-1);
	    continue;
	}
	if (lexer->element_type(cname) >= 0)
	    errh->lerror(lexer->element_landmark(which), "%<%s%> is an element class", cname.c_str());
	if (_elements[i] == TUNNEL_TYPE) {
	    eidx_map.resize(eidx_map.size() + 3);
	    // probably should assert() next 2 elements are tunnel type and
	    // have the expected names
	    lexer->add_tunnels(cname, eidx_map.end() - 3);
	    i += 2;
	} else {
	    eidx = lexer->get_element(cname, _elements[i], cp_expand(_element_configurations[i], ve), _element_filenames[i], _element_linenos[i]);
	    eidx_map.push_back(eidx);
	}
    }

    // now copy hookups
    for (const Connection *cp = _conn.begin(); cp != _conn.end(); ++cp)
	if (eidx_map[(*cp)[0].idx] >= 0 && eidx_map[(*cp)[0].idx] >= 0)
	    lexer->_c->connect(eidx_map[(*cp)[1].idx], (*cp)[1].port,
			       eidx_map[(*cp)[0].idx], (*cp)[0].port);

    // now expand those
    for (int i = 2; i < eidx_map.size(); i++)
	if (eidx_map[i] >= 0)
	    lexer->expand_compound_element(eidx_map[i], ve);
}

//
// LEXER
//

Lexer::FileState::FileState(const String &data, const String &filename)
  : _big_string(data), _end(data.end()), _pos(data.begin()),
    _filename(filename ? filename : String::make_stable("config", 6)),
    _original_filename(_filename), _lineno(1)
{
}

Lexer::Lexer()
  : _file(String(), String()), _lextra(0), _unlex_pos(0),
    _element_type_map(-1),
    _last_element_type(ET_NULL), _free_element_type(-1),
    _global_scope(0), _element_map(-1), _c(0),
    _errh(ErrorHandler::default_handler())
{
  end_parse(ET_NULL);		// clear private state
  ADD_ELEMENT_TYPE("<tunnel>", error_element_factory, 0, false);
  ADD_ELEMENT_TYPE("Error", error_element_factory, 0, false);
  assert(element_type("<tunnel>") == TUNNEL_TYPE && element_type("Error") == ERROR_TYPE);
}

Lexer::~Lexer()
{
  end_parse(ET_NULL);

  // get rid of nonscoped element types
  for (int t = 0; t < _element_types.size(); t++)
    if (_element_types[t].factory == compound_element_factory) {
      Lexer::Compound *compound = (Lexer::Compound *) _element_types[t].thunk;
      delete compound;
    }
}

int
Lexer::begin_parse(const String &data, const String &filename,
		   LexerExtra *lextra, ErrorHandler *errh)
{
  _file = FileState(data, filename);
  _compact_config = false;

  _c = new Compound("", "", 0);
  _group_depth = 0;

  _lextra = lextra;
  _errh = (errh ? errh : ErrorHandler::default_handler());

  return lexical_scoping_in();
}

void
Lexer::end_parse(int cookie)
{
  lexical_scoping_out(cookie);

  for (TunnelEnd **tep = _tunnels.begin(); tep != _tunnels.end(); ++tep)
    while (TunnelEnd *t = *tep) {
      *tep = t->next();
      delete t;
    }
  _tunnels.clear();

  delete _c;
  _c = 0;

  _element_map.clear();
  _requirements.clear();
  _libraries.clear();

  _file = FileState(String(), String());
  _lextra = 0;

  // also free out Strings held in the _unlex buffer
  for (int i = 0; i < UNLEX_SIZE; ++i)
      _unlex[i] = Lexeme();
  _unlex_pos = 0;

  _anonymous_offset = 0;

  _errh = ErrorHandler::default_handler();
}


// LEXING: LOWEST LEVEL

String
Lexer::remaining_text() const
{
  return _file._big_string.substring(_file._pos, _file._big_string.end());
}

void
Lexer::set_remaining_text(const String &s)
{
  _file._big_string = s;
  _file._pos = s.begin();
  _file._end = s.end();
}

const char *
Lexer::FileState::skip_line(const char *s)
{
  _lineno++;
  for (; s < _end; s++)
    if (*s == '\n')
      return s + 1;
    else if (*s == '\r') {
      if (s + 1 < _end && s[1] == '\n')
	return s + 2;
      else
	return s + 1;
    }
  _lineno--;
  return s;
}

const char *
Lexer::FileState::skip_slash_star(const char *s)
{
  for (; s < _end; s++)
    if (*s == '\n')
      _lineno++;
    else if (*s == '\r') {
      if (s + 1 < _end && s[1] == '\n')
	s++;
      _lineno++;
    } else if (*s == '*' && s + 1 < _end && s[1] == '/')
      return s + 2;
  return _end;
}

const char *
Lexer::FileState::skip_backslash_angle(const char *s)
{
  for (; s < _end; s++)
    if (*s == '\n')
      _lineno++;
    else if (*s == '\r') {
      if (s + 1 < _end && s[1] == '\n')
	s++;
      _lineno++;
    } else if (*s == '/' && s + 1 < _end) {
      if (s[1] == '/')
	s = skip_line(s + 2) - 1;
      else if (s[1] == '*')
	s = skip_slash_star(s + 2) - 1;
    } else if (*s == '>')
      return s + 1;
  return _end;
}

const char *
Lexer::FileState::skip_quote(const char *s, char endc)
{
  for (; s < _end; s++)
    if (*s == '\n')
      _lineno++;
    else if (*s == '\r') {
      if (s + 1 < _end && s[1] == '\n')
	s++;
      _lineno++;
    } else if (*s == '\\' && endc == '\"' && s + 1 < _end) {
      if (s[1] == '<')
	s = skip_backslash_angle(s + 2) - 1;
      else if (s[1] == '\"')
	s++;
    } else if (*s == endc)
      return s + 1;
  return _end;
}

const char *
Lexer::FileState::process_line_directive(const char *s, Lexer *lexer)
{
  for (s++; s < _end && (*s == ' ' || *s == '\t'); s++)
    /* nada */;
  if (s + 4 < _end && *s == 'l' && s[1] == 'i'
      && s[2] == 'n' && s[3] == 'e'
      && (s[4] == ' ' || s[4] == '\t')) {
    for (s += 5; s < _end && (*s == ' ' || *s == '\t'); s++)
      /* nada */;
  }
  if (s >= _end || !isdigit((unsigned char) *s)) {
    // complain about bad directive
    lexer->lerror("unknown preprocessor directive");
    return skip_line(s);
  }

  // parse line number
  for (_lineno = 0; s < _end && isdigit((unsigned char) *s); s++)
    _lineno = _lineno * 10 + *s - '0';
  _lineno--;			// account for extra line

  for (; s < _end && (*s == ' ' || *s == '\t'); s++)
    /* nada */;
  if (s < _end && *s == '\"') {
    // parse filename
    const char *first_in_filename = s;
    for (s++; s < _end && *s != '\"' && *s != '\n' && *s != '\r'; s++)
      if (*s == '\\' && s + 1 < _end && s[1] != '\n' && s[1] != '\r')
	s++;
    _filename = cp_unquote(_big_string.substring(first_in_filename, s) + "\"");
    // an empty filename means return to the input file's name
    if (!_filename)
      _filename = _original_filename;
  }

  // reach end of line
  for (; s < _end && *s != '\n' && *s != '\r'; s++)
    /* nada */;
  if (s + 1 < _end && *s == '\r' && s[1] == '\n')
    s++;
  return s;
}

Lexeme
Lexer::FileState::next_lexeme(Lexer *lexer)
{
  const char *s = _pos;
  while (true) {
    while (s < _end && isspace((unsigned char) *s)) {
      if (*s == '\n')
	_lineno++;
      else if (*s == '\r') {
	if (s + 1 < _end && s[1] == '\n')
	  s++;
	_lineno++;
      }
      s++;
    }
    if (s >= _end) {
      _pos = _end;
      return Lexeme();
    } else if (*s == '/' && s + 1 < _end) {
      if (s[1] == '/')
	s = skip_line(s + 2);
      else if (s[1] == '*')
	s = skip_slash_star(s + 2);
      else
	break;
    } else if (*s == '#' && (s == _big_string.begin() || s[-1] == '\n' || s[-1] == '\r'))
      s = process_line_directive(s, lexer);
    else
      break;
  }

  const char *word_pos = s;

  // find length of current word
  if (isalnum((unsigned char) *s) || *s == '_' || *s == '@') {
   more_word_characters:
    s++;
    while (s < _end && (isalnum((unsigned char) *s) || *s == '_' || *s == '@'))
      s++;
    if (s + 1 < _end && *s == '/' && (isalnum((unsigned char) s[1]) || s[1] == '_' || s[1] == '@'))
      goto more_word_characters;
    _pos = s;
    String word = _big_string.substring(word_pos, s);
    if (word.equals("elementclass", 12))
      return Lexeme(lexElementclass, word);
    else if (word.equals("require", 7))
      return Lexeme(lexRequire, word);
    else if (word.equals("provide", 7))
      return Lexeme(lexProvide, word);
    else if (word.equals("define", 6))
      return Lexeme(lexDefine, word);
    else
      return Lexeme(lexIdent, word, lexer->_compact_config);
  }

  // check for variable
  if (*s == '$') {
    s++;
    while (s < _end && (isalnum((unsigned char) *s) || *s == '_'))
      s++;
    if (s + 1 > word_pos) {
      _pos = s;
      return Lexeme(lexVariable, _big_string.substring(word_pos + 1, s), lexer->_compact_config);
    } else
      s--;
  }

  if (s + 1 < _end) {
    if (*s == '-' && s[1] == '>') {
      _pos = s + 2;
      return Lexeme(lexArrow, _big_string.substring(s, s + 2));
    } else if (*s == '=' && s[1] == '>') {
      _pos = s + 2;
      return Lexeme(lex2Arrow, _big_string.substring(s, s + 2));
    } else if (*s == ':' && s[1] == ':') {
      _pos = s + 2;
      return Lexeme(lex2Colon, _big_string.substring(s, s + 2));
    } else if (*s == '|' && s[1] == '|') {
      _pos = s + 2;
      return Lexeme(lex2Bar, _big_string.substring(s, s + 2));
    }
  }
  if (s + 2 < _end && *s == '.' && s[1] == '.' && s[2] == '.') {
    _pos = s + 3;
    return Lexeme(lex3Dot, _big_string.substring(s, s + 3));
  }

  _pos = s + 1;
  return Lexeme(*s, _big_string.substring(s, s + 1));
}

String
Lexer::FileState::lex_config(Lexer *lexer)
{
  const char *config_pos = _pos;
  const char *s = _pos;
  unsigned paren_depth = 1;

  for (; s < _end; s++)
    if (*s == '(')
      paren_depth++;
    else if (*s == ')') {
      paren_depth--;
      if (!paren_depth)
	break;
    } else if (*s == '\n')
      _lineno++;
    else if (*s == '\r') {
      if (s + 1 < _end && s[1] == '\n')
	s++;
      _lineno++;
    } else if (*s == '/' && s + 1 < _end) {
      if (s[1] == '/')
	s = skip_line(s + 2) - 1;
      else if (s[1] == '*')
	s = skip_slash_star(s + 2) - 1;
    } else if (*s == '\'' || *s == '\"')
      s = skip_quote(s + 1, *s) - 1;
    else if (*s == '\\' && s + 1 < _end && s[1] == '<')
      s = skip_backslash_angle(s + 2) - 1;

  _pos = s;
  String r = _big_string.substring(config_pos, s);
  return lexer->_compact_config ? r.compact() : r;
}

String
Lexer::lexeme_string(int kind)
{
    static const char names[] = "identifier\0variable\0'->'\0'=>'\0"
	"'::'\0'||'\0'...'\0'elementclass'\0'require'\0'provide'\0"
	"'define'";
    static const uint8_t offsets[] = {
	0, 11, 20, 25, 30, 35, 40, 46, 61, 71, 81, 90
    };
    static_assert(sizeof(names) == 90);

    char buf[14];
    if (kind >= lexIdent && kind < lexIdent + (int) sizeof(offsets) - 1) {
	const uint8_t *op = offsets + (kind - lexIdent);
	return String::make_stable(names + op[0], op[1] - op[0] - 1);
    } else if (kind >= 32 && kind < 127) {
	sprintf(buf, "'%c'", kind);
	return buf;
    } else {
	sprintf(buf, "'\\%03d'", kind);
	return buf;
    }
}


// LEXING: MIDDLE LEVEL (WITH PUSHBACK)

bool
Lexer::expect(int kind, bool no_error)
{
  if (_unlex_pos) {
    if (_unlex[_unlex_pos - 1].is(kind)) {
      --_unlex_pos;
      return true;
    }
  } else {
    // Never adds to _unlex, which requires a nonobvious implementation.
    String old_filename = _file._filename;
    unsigned old_lineno = _file._lineno;
    const char *old_pos = _file._pos;
    if (lex().is(kind))
      return true;
    _file._filename = old_filename;
    _file._lineno = old_lineno;
    _file._pos = old_pos;
  }
  if (!no_error)
    lerror("expected %s", lexeme_string(kind).c_str());
  return false;
}


// ERRORS

String
Lexer::FileState::landmark() const
{
    return Compound::landmark_string(_filename, _lineno);
}

int
Lexer::lerror(const char *format, ...)
{
  va_list val;
  va_start(val, format);
  _errh->xmessage(_file.landmark(), ErrorHandler::e_error, format, val);
  va_end(val);
  return -1;
}


// ELEMENT TYPES

int
Lexer::add_element_type(const String &name, ElementFactory factory, uintptr_t thunk,
#ifdef CLICK_LINUXMODULE
			struct module *module,
#endif
			bool scoped)
{
  assert(factory);	       // 3.Sep.2003: anonymous compounds have name ""
  int tid;
  if (_free_element_type < 0) {
    tid = _element_types.size();
    _element_types.push_back(ElementType());
  } else {
    tid = _free_element_type;
    _free_element_type = _element_types[tid].next;
  }
  _element_types[tid].factory = factory;
  _element_types[tid].thunk = thunk;
#ifdef CLICK_LINUXMODULE
  _element_types[tid].module = module;
#endif
  _element_types[tid].name = name;
  _element_types[tid].next = _last_element_type | (scoped ? (int)ET_SCOPED : 0);
  if (name)
    _element_type_map.set(name, tid);
  _last_element_type = tid;
  return tid;
}

int
Lexer::force_element_type(String name, bool report_error)
{
  int ftid = element_type(name);
  if (ftid >= 0)
    return ftid;
  if (report_error)
    lerror("unknown element class %<%s%>", name.c_str());
  return ADD_ELEMENT_TYPE(name, error_element_factory, 0, true);
}

int
Lexer::lexical_scoping_in() const
{
  return _last_element_type;
}

void
Lexer::lexical_scoping_out(int last)
{
  int *prev = &_last_element_type;
  while (*prev != last && *prev != ET_NULL) {
    assert(!(*prev & ET_SCOPED));
    int *next = &_element_types[*prev].next;
    if (*next & ET_SCOPED)
      remove_element_type(*prev, prev);
    else
      prev = next;
  }
}

int
Lexer::remove_element_type(int removed, int *prev_hint)
{
  // exit early if trying to remove bad type
  if (removed < 0 || removed >= _element_types.size() || _element_types[removed].factory == 0)
    return -1;

  // fix _element_type_next chain
  if (!prev_hint || (int)(*prev_hint & ET_TMASK) != removed)
    for (prev_hint = &_last_element_type;
	 (*prev_hint & ET_TMASK) != ET_NULL && (int)(*prev_hint & ET_TMASK) != removed;
	 prev_hint = &_element_types[*prev_hint & ET_TMASK].next)
      /* nada */;
  assert(prev_hint);
  if ((int)(*prev_hint & ET_TMASK) == removed)
    *prev_hint = (*prev_hint & ~ET_TMASK) | (_element_types[removed].next & ET_TMASK);

  // fix up element type name map
  const String &name = _element_types[removed].name;
  if (name && element_type(name) == removed) {
    int trav;
    for (trav = _element_types[removed].next & ET_TMASK;
	 trav != ET_NULL && _element_types[trav].name != name;
	 trav = _element_types[trav].next & ET_TMASK)
      /* nada */;
    if (trav == ET_NULL)
	_element_type_map.erase(name);
    else
	_element_type_map.set(name, trav);
  }

  // remove stuff
  if (_element_types[removed].factory == compound_element_factory) {
    Lexer::Compound *compound = (Lexer::Compound *) _element_types[removed].thunk;
    delete compound;
  }
  _element_types[removed].factory = 0;
  _element_types[removed].name = String();
  _element_types[removed].next = _free_element_type;
  _free_element_type = removed;

  return 0;
}

void
Lexer::element_type_names(Vector<String> &v) const
{
  for (HashTable<String, int>::const_iterator i = _element_type_map.begin(); i.live(); i++)
    if (i.value() >= 0 && i.key() != "<tunnel>")
      v.push_back(i.key());
}


// PORT TUNNELS

void
Lexer::add_tunnels(String name, int *eidxes)
{
    String names[4];
    names[0] = names[3] = name;
    names[1] = name + "/" + port_names[0];
    names[2] = name + "/" + port_names[1];

    Port ports[4];
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
	ports[i].idx = eidxes[i] = get_element(names[i], TUNNEL_TYPE);
	ports[i].port = 0;
	if (_c->_elements[eidxes[i]] != TUNNEL_TYPE) {
	    redeclaration_error(_errh, "element", names[i], _file.landmark(), _c->element_landmark(ports[i].idx));
	    ok = false;
	}
    }
    ports[3] = ports[0];

    if (ok && _c->depth() == 0) {
	TunnelEnd *tes[4];
	for (int i = 0; i < 4; ++i) {
	    tes[i] = find_tunnel(ports[i], i % 2, true);
	    if (tes[i]->other()) {
		redeclaration_error(_errh, "connection tunnel", names[i], _file.landmark(), _c->element_landmark(ports[i].idx));
		ok = false;
	    }
	}
	if (ok) {
	    tes[0]->pair_with(tes[1]);
	    tes[2]->pair_with(tes[3]);
	}
    }
}

// ELEMENTS

int
Lexer::get_element(String name, int etype, const String &conf,
		   const String &filename, unsigned lineno)
{
  assert(name && etype >= 0 && etype < _element_types.size());

  // if an element 'name' already exists return it
  if (_element_map[name] >= 0)
    return _element_map[name];

  int eid = _c->_elements.size();
  _element_map.set(name, eid);

  // check 'name' for validity
  for (int i = 0; i < name.length(); i++) {
    bool ok = false;
    for (; i < name.length() && name[i] != '/'; i++)
      if (!isdigit((unsigned char) name[i]))
	ok = true;
    if (!ok) {
      lerror("element name %<%s%> has all-digit component", name.c_str());
      break;
    }
  }

  _c->_element_names.push_back(name);
  _c->_element_configurations.push_back(conf);
  if (!filename && !lineno) {
      _c->_element_filenames.push_back(_file._filename);
      _c->_element_linenos.push_back(_file._lineno);
  } else {
      _c->_element_filenames.push_back(filename);
      _c->_element_linenos.push_back(lineno);
  }
  _c->_elements.push_back(etype);
  _c->_element_nports[0].push_back(0);
  _c->_element_nports[1].push_back(0);
  return eid;
}

String
Lexer::anon_element_name(const String &class_name) const
{
  int anonymizer = _c->_elements.size() - _anonymous_offset + 1;
  return ";" + class_name + "@" + String(anonymizer);
}

String
Lexer::deanonymize_element_name(const String &ename, int eidx)
{
    // This function uses _element_map.
    assert(ename && ename[0] == ';');
    String name = ename.substring(1);
    if (_element_map[name] >= 0) {
	int at_pos = name.find_right('@');
	assert(at_pos >= 0);
	String prefix = name.substring(0, at_pos + 1);
	const char *abegin = name.begin() + at_pos + 1, *aend = abegin;
	while (aend < name.end() && isdigit((unsigned char) *aend))
	    ++aend;
	int anonymizer;
	cp_integer(abegin, aend, 10, &anonymizer);
	do {
	    anonymizer++;
	    name = prefix + String(anonymizer);
	} while (_element_map[name] >= 0);
    }
    _element_map.set(name, eidx);
    return name;
}

String
Lexer::element_name(int eid) const
{
  if (eid < 0 || eid >= _c->_elements.size())
    return "##no-such-element##";
  else if (_c->_element_names[eid])
    return _c->_element_names[eid];
  else {
    char buf[100];
    sprintf(buf, "@%d", eid);
    int t = _c->_elements[eid];
    if (t == TUNNEL_TYPE)
      return "<tunnel" + String(buf) + ">";
    else if (!_element_types[t].factory)
      return "<null" + String(buf) + ">";
    else
      return _element_types[t].name + String(buf);
  }
}

String
Lexer::element_landmark(int eid) const
{
    if (eid < 0 || eid >= _c->_elements.size())
	return String::make_stable("##no-such-element##");
    else if (String s = _c->element_landmark(eid))
	return s;
    else
	return String::make_stable("<unknown>");
}


// PARSING

bool
Lexer::yport(Vector<int> &ports)
{
    if (!expect('[', true))
	return false;

    int nports = ports.size();
    while (1) {
	Lexeme t = lex();
	if (t.is(lexIdent)) {
	    int port;
	    if (!cp_integer(t.string(), &port)) {
		lerror("syntax error: port number should be integer");
		port = 0;
	    }
	    ports.push_back(port);
	} else if (t.is(']')) {
	    if (nports == ports.size())
		ports.push_back(0);
	    ports.push_back(-1);
	    return true;
	} else {
	    lerror("syntax error: expected port number");
	    unlex(t);
	    return ports.size() != nports;
	}

	t = lex();
	if (t.is(']'))
	    return true;
	else if (!t.is(',')) {
	    lerror("syntax error: expected ','");
	    unlex(t);
	}
    }
}

namespace {
struct ElementState {
    String name;
    int type;
    int decl_type;
    bool bare;
    String configuration;
    String filename;
    unsigned lineno;
    ElementState *next;
    ElementState(const String &name_, int type_, bool bare_,
		 const String &filename_, unsigned lineno_,
		 ElementState **&tail)
	: name(name_), type(type_), decl_type(-1), bare(bare_),
	  filename(filename_), lineno(lineno_), next(0) {
	*tail = this;
	tail = &next;
    }
};
}

// Returned result is a vector listing all elements and port references.
// The vector is a concatenated series of groups, each of which looks like:
// group[0] element index
// group[1] number of input ports
// group[2] number of output ports
// group[3...3+group[1]] input ports
// group[3+group[1]...3+group[1]+group[2]] output ports
bool
Lexer::yelement(Vector<int> &result, bool in_allowed)
{
    ElementState *head = 0, **tail = &head;
    Vector<int> res;
    bool any_implicit = false, any_ports = false;

    // parse lists of names (which might include classes)
    Lexeme t;
    while (1) {
	int esize = res.size();
	res.push_back(-1);
	res.push_back(0);
	res.push_back(0);
	bool this_implicit = false;

	// initial port
	yport(res);
	res[esize + 1] = res.size() - (esize + 3);

	// element name or class
	String name;
	int type;

	t = lex();
	if (t.is(lexIdent)) {
	    name = t.string();
	    type = element_type(name);
	} else if (t.is('{')) {
	    type = ycompound();
	    name = _element_types[type].name;
	} else if (t.is('(')) {
	    name = anon_element_name("");
	    type = -1;
	    int group_nports[2];
	    ygroup(name, group_nports);

	    // an anonymous group has implied, overridable port
	    // specifications on both sides for all inputs & outputs
	    for (int k = 0; k < 2; ++k)
		if (res[esize + 1 + k] == 0) {
		    res[esize + 1 + k] = group_nports[k];
		    for (int i = 0; i < group_nports[k]; ++i)
			res.push_back(i);
		}
	} else {
	    bool nested = _c->depth() || _group_depth;
	    if (nested && (t.is(lexArrow) || t.is(lex2Arrow)))
		this_implicit = !in_allowed && (res[esize + 1] || !esize);
	    else if (nested && t.is(','))
		this_implicit = !!res[esize + 1];
	    else if (nested && !t.is(lex2Colon))
		this_implicit = in_allowed && (res[esize + 1] || !esize);
	    if (this_implicit) {
		any_implicit = true;
		name = port_names[in_allowed];
		type = element_type(name);
		if (!in_allowed)
		    click_swap(res[esize+1], res[esize+2]);
		unlex(t);
	    } else {
		if (res[esize + 1])
		    lerror("stranded port ignored");
		res.resize(esize);
		if (esize == 0) {
		    if (in_allowed)
			unlex(t);
		    else
			lerror("syntax error near %<%#s%>", t.string().c_str());
		    return false;
		}
		break;
	    }
	}

	ElementState *e = new ElementState(name, type, t.is(lexIdent), _file._filename, _file._lineno, tail);

	// ":: CLASS" declaration
	t = lex();
	if (t.is(lex2Colon) && !this_implicit) {
	    t = lex();
	    if (t.is(lexIdent))
		e->decl_type = force_element_type(t.string());
	    else if (t.is('{'))
		e->decl_type = ycompound();
	    else {
		lerror("missing element type in declaration");
		e->decl_type = force_element_type(e->name);
		unlex(t);
	    }
	    e->bare = false;
	    t = lex();
	}

	// configuration string
	if (t.is('(') && !this_implicit) {
	    if (_element_map[e->name] >= 0)
		lerror("configuration string ignored on element reference");
	    e->configuration = lex_config();
	    expect(')');
	    e->bare = false;
	    t = lex();
	}

	// final port
	if (t.is('[') && !this_implicit) {
	    unlex(t);
	    if (res[esize + 2])	// delete any implied ports
		res.resize(esize + 3 + res[esize + 1]);
	    yport(res);
	    res[esize + 2] = res.size() - (esize + 3 + res[esize + 1]);
	    t = lex();
	}
	any_ports = any_ports || res[esize + 1] || res[esize + 2];

	if (!t.is(','))
	    break;
    }

    unlex(t);

    // maybe complain about implicits
    if (any_implicit && in_allowed && (t.is(lexArrow) || t.is(lex2Arrow)))
	lerror("implicit ports used in the middle of a chain");

    // maybe spread class and configuration for standalone
    // multiple-element declaration
    if (head->next && !in_allowed && !(t.is(lexArrow) || t.is(lex2Arrow))
	&& !any_ports && !any_implicit) {
	ElementState *last = head;
	while (last->next && last->bare)
	    last = last->next;
	if (!last->next && last->decl_type)
	    for (ElementState *e = head; e->next; e = e->next) {
		e->decl_type = last->decl_type;
		e->configuration = last->configuration;
	    }
    }

    // add elements
    int *resp = res.begin();
    while (ElementState *e = head) {
	if (e->type >= 0 || (*resp = _element_map[e->name]) < 0) {
	    if (e->decl_type >= 0 && e->type >= 0)
		_errh->lerror(Compound::landmark_string(e->filename, e->lineno), "class %<%s%> used as element name", e->name.c_str());
	    else if (e->decl_type < 0 && e->type < 0) {
		_errh->lerror(Compound::landmark_string(e->filename, e->lineno), "undeclared element %<%s%>, assuming element class", e->name.c_str());
		e->type = force_element_type(e->name, false);
	    }
	    if (e->type >= 0)
		e->name = anon_element_name(e->name);
	    *resp = get_element(e->name, e->type >= 0 ? e->type : e->decl_type, e->configuration, e->filename, e->lineno);
	} else if (e->decl_type >= 0) {
	    _errh->lerror(Compound::landmark_string(e->filename, e->lineno), "redeclaration of element %<%s%>", e->name.c_str());
	    if (_c->_elements[*resp] != TUNNEL_TYPE)
		_errh->lerror(_c->element_landmark(*resp), "element %<%s%> previously declared here", e->name.c_str());
	}

	resp += 3 + resp[1] + resp[2];
	head = e->next;
	delete e;
    }

    result.swap(res);
    return true;
}

void
Lexer::yconnection_check_useless(const Vector<int> &x, bool isoutput)
{
    for (const int *it = x.begin(); it != x.end(); it += 3 + it[1] + it[2])
	if (it[isoutput ? 2 : 1] > 0) {
	    lerror(isoutput ? "output ports ignored at end of chain" : "input ports ignored at start of chain");
	    break;
	}
}

void
Lexer::yconnection_analyze_ports(const Vector<int> &x, bool isoutput,
				 int &min_ports, int &expandable)
{
    min_ports = expandable = 0;
    for (const int *it = x.begin(); it != x.end(); it += 3 + it[1] + it[2]) {
	int n = it[isoutput ? 2 : 1];
	if (n <= 1)
	    min_ports += 1;
	else if (it[3 + (isoutput ? it[1] : 0) + n - 1] == -1) {
	    min_ports += n - 1;
	    ++expandable;
	} else
	    min_ports += n;
    }
}

void
Lexer::yconnection_connect_all(Vector<int> &outputs, Vector<int> &inputs,
			       int connector)
{
    int minp[2];
    int expandable[2];
    yconnection_analyze_ports(outputs, true, minp[1], expandable[1]);
    yconnection_analyze_ports(inputs, false, minp[0], expandable[0]);

    if (expandable[0] + expandable[1] > 1) {
	lerror("at most one expandable port allowed per connection");
	expandable[minp[0] < minp[1]] = 0;
    }

    if (connector == lex2Arrow)
	// '=>' can interpret missing ports as expandable ports
	for (int k = 0; k < 2; ++k) {
	    Vector<int> &myvec(k ? outputs : inputs);
	    if (minp[k] == 1 && minp[1-k] > 1 && myvec[1+k] == 0)
		expandable[k] = 1;
	}

    bool step[2];
    int nexpandable[2];
    for (int k = 0; k < 2; ++k) {
	step[k] = minp[k] > 1 || expandable[k];
	nexpandable[k] = expandable[k] ? minp[1-k] - minp[k] : 0;
    }

    if (step[0] && step[1]) {
	if (connector != lex2Arrow)
	    lerror("syntax error: many-to-many connections require %<=>%>");
	if (!expandable[0] && !expandable[1] && minp[0] != minp[1])
	    lerror("connection mismatch: %d outputs connected to %d inputs", minp[1], minp[0]);
	else if (!expandable[0] && minp[0] < minp[1])
	    lerror("connection mismatch: %d or more outputs connected to %d inputs", minp[1], minp[0]);
	else if (!expandable[1] && minp[1] < minp[0])
	    lerror("connection mismatch: %d outputs connected to %d or more inputs", minp[1], minp[0]);
    } else if (!step[0] && !step[1])
	step[0] = true;

    const int *it[2] = {inputs.begin(), outputs.begin()};
    int ppos[2] = {0, 0}, port[2] = {-1, -1};
    while (it[0] != inputs.end() && it[1] != outputs.end()) {
	for (int k = 0; k < 2; ++k)
	    if (port[k] < 0) {
		int np = it[k][1+k];
		port[k] = np ? it[k][3 + (k ? it[k][1] : 0)] : 0;
	    }

	_c->connect(it[1][0], port[1], it[0][0], port[0]);

	for (int k = 0; k < 2; ++k)
	    if (step[k]) {
		int np = it[k][1+k];
		const int *pvec = it[k] + 3 + (k ? it[k][1] : 0);
		++ppos[k];
		if (ppos[k] < np && pvec[ppos[k]] >= 0)
		    // port list
		    port[k] = pvec[ppos[k]];
		else if (np && pvec[np-1] == -1 && nexpandable[k] > 0) {
		    // expandable port
		    port[k] = pvec[np-2] + ppos[k] - (np-2);
		    --nexpandable[k];
		} else if (np == 0 && minp[k] == 1 && nexpandable[k] > 0) {
		    // missing port interpreted as expandable port
		    port[k] = ppos[k];
		    --nexpandable[k];
		} else {
		    // next element in comma-separated list
		    port[k] = -1;
		    ppos[k] = 0;
		    it[k] += 3 + it[k][1] + it[k][2];
		}
	    }
    }
}

bool
Lexer::yconnection()
{
    Vector<int> elements1, elements2;
    int connector = 0;
    Lexeme t;

    while (true) {
	// get element
	elements2.clear();
	if (!yelement(elements2, !elements1.empty())) {
	    yconnection_check_useless(elements1, true);
	    return !elements1.empty();
	}

	if (elements1.empty())
	    yconnection_check_useless(elements2, false);
	else
	    yconnection_connect_all(elements1, elements2, connector);

    relex:
	t = lex();
	switch (t.kind()) {

	case ',':
	case lex2Colon:
	    lerror("syntax error before %<%#s%>", t.string().c_str());
	    goto relex;

	case lexArrow:
	case lex2Arrow:
	    connector = t.kind();
	    break;

	case lexIdent:
	case '{':
	case '}':
	case '[':
	case ')':
	case lex2Bar:
	case lexElementclass:
	case lexRequire:
	case lexProvide:
	case lexDefine:
	    unlex(t);
	    // FALLTHRU
	case ';':
	case lexEOF:
	    yconnection_check_useless(elements2, true);
	    return true;

	default:
	    lerror("syntax error near %<%#s%>", t.string().c_str());
	    if (t.kind() >= lexIdent)	// save meaningful tokens
		unlex(t);
	    return true;

	}

	// have 'x ->'
	elements1.swap(elements2);
    }
}

void
Lexer::yelementclass()
{
  Lexeme tname = lex();
  String name;
  if (tname.is(lexIdent))
    name = tname.string();
  else {
    unlex(tname);
    lerror("expected element type name");
  }

  Lexeme tnext = lex();
  if (tnext.is('{'))
    ycompound(name);

  else if (tnext.is(lexIdent)) {
    // define synonym type
    int t = force_element_type(tnext.string());
    ADD_ELEMENT_TYPE(name, _element_types[t].factory, _element_types[t].thunk, true);

  } else {
    lerror("syntax error near %<%#s%>", tnext.string().c_str());
    ADD_ELEMENT_TYPE(name, error_element_factory, 0, true);
  }
}

void
Lexer::ycompound_arguments(Compound *comptype)
{
  Lexeme t1, t2;

  while (1) {
    String vartype, varname;

    // read "IDENTIFIER $VARIABLE" or "$VARIABLE"
    t1 = lex();
    if (t1.is(lexIdent)) {
      t2 = lex();
      if (t2.is(lexVariable)) {
	vartype = t1.string();
	varname = t2.string();
      } else {
	if (comptype->scope().size() > 0)
	  lerror("expected variable");
	unlex(t2);
	unlex(t1);
	break;
      }
    } else if (t1.is(lexVariable))
      varname = t1.string();
    else if (t1.is('|'))
      break;
    else {
      if (comptype->nformals() > 0)
	lerror("expected variable");
      unlex(t1);
      break;
    }

    comptype->define(varname, vartype, true, this);

    Lexeme tsep = lex();
    if (tsep.is('|'))
      break;
    else if (!tsep.is(',')) {
      lerror("expected %<,%> or %<|%>");
      unlex(tsep);
      break;
    }
  }
}

int
Lexer::ycompound(String name)
{
  HashTable<String, int> old_element_map(-1);
  old_element_map.swap(_element_map);
  HashTable<String, int> old_type_map(_element_type_map);
  int old_offset = _anonymous_offset;

  Compound *first = 0, *last = 0;
  int extension = -1;

  while (1) {
    Lexeme dots = lex();
    if (dots.is(lex3Dot)) {
      // '...' marks an extension type
      if (element_type(name) < 0) {
	lerror("cannot extend unknown element class %<%s%>", name.c_str());
	ADD_ELEMENT_TYPE(name, error_element_factory, 0, true);
      }
      extension = element_type(name);

      dots = lex();
      if (!first || !dots.is('}'))
	lerror("%<...%> should occur last, after one or more compounds");
      if (dots.is('}') && first)
	break;
    }
    unlex(dots);

    // create a compound
    _element_map.clear();
    Compound *old_c = _c;
    Compound *ct = _c = new Compound(name, _file.landmark(), &_c->_scope);
    get_element("input", TUNNEL_TYPE);
    get_element("output", TUNNEL_TYPE);
    _anonymous_offset = 2;

    ycompound_arguments(ct);
    while (ystatement('}'))
      /* nada */;

    _anonymous_offset = old_offset;
    _element_type_map = old_type_map;
    _c = old_c;

    ct->finish(this, _errh);

    if (last) {
      int t = ADD_ELEMENT_TYPE(name, compound_element_factory, (uintptr_t) ct, true);
      last->set_overload_type(t);
    } else
      first = ct;
    last = ct;

    // check for '||' or '}'
    if (!lex().is(lex2Bar))
      break;
  }

  // on the way out
  old_element_map.swap(_element_map);

  // add all types to ensure they're freed later
  if (extension)
    last->set_overload_type(extension);
  return ADD_ELEMENT_TYPE(name, compound_element_factory, (uintptr_t) first, true);
}

void
Lexer::ygroup(String name, int group_nports[2])
{
    int eidexes[3];
    add_tunnels(name, eidexes);

    int old_input = _element_map["input"];
    int old_output = _element_map["output"];
    _element_map["input"] = eidexes[1];
    _element_map["output"] = eidexes[2];
    ++_group_depth;

    while (ystatement(')'))
	/* nada */;
    expect(')');

    // check that all inputs and outputs are used
    LandmarkErrorHandler lerrh(_errh, _file.landmark());
    const char *printable_name = (name[0] == ';' ? "<anonymous group>" : name.c_str());
    group_nports[0] = _c->check_pseudoelement(eidexes[1], false, printable_name, &lerrh);
    group_nports[1] = _c->check_pseudoelement(eidexes[2], true, printable_name, &lerrh);

    --_group_depth;
    _element_map["input"] = old_input;
    _element_map["output"] = old_output;
}

void
Lexer::yrequire_library(const String &value)
{
#if CLICK_USERLEVEL
    assert(!_unlex_pos);
    if (_c->depth()) {
	lerror("%<require library%> must be used at file scope");
	return;
    }

    String dir = _file._filename;
    int pos = dir.find_right('/');
    if (pos > 0)
	dir = dir.substring(0, pos);
    else
	dir = ".";
    String fn = clickpath_find_file(value, "conf", dir, 0);
    if (!fn) {
	lerror("library %<%#s%> not found in CLICKPATH/conf", fn.c_str());
	return;
    }

    for (String *it = _libraries.begin(); it != _libraries.end(); ++it)
	if (*it == fn)
	    return;
    _libraries.push_back(fn);

    LandmarkErrorHandler lerrh(_errh, _file.landmark());
    int before = lerrh.nerrors();
    String data = file_string(fn, &lerrh);
    if (lerrh.nerrors() != before)
	return;

    FileState old_file(_file);
    _file = FileState(data, fn);
    while (ystatement(0))
	/* do nothing */;
    _file = old_file;
#else
    (void) value;
    lerror("%<require library%> may not be used in this driver");
#endif
}

void
Lexer::yrequire()
{
    if (!expect('('))
	return;

    String requirement = lex_config();
    expect(')');
    // pre-read ';' to make it easier to write parsing extensions
    expect(';', true);

    Vector<String> args;
    cp_argvec(requirement, args);

    String compact_config_str = String::make_stable("compact_config", 14);
    String package_str = String::make_stable("package", 7);
    String library_str = String::make_stable("library", 7);

    for (int i = 0; i < args.size(); i++) {
	Vector<String> words;
	cp_spacevec(args[i], words);
	if (words.size() == 0)
	    continue;		// do nothing

	String type, value;
	(void) cp_word(words[0], &type);
	// "require(UNKNOWN)" means "require(package UNKNOWN)"
	if (type && type != compact_config_str && type != package_str
	    && type != library_str && words.size() == 1) {
	    words.push_back(type);
	    type = package_str;
	}

	if (type == compact_config_str && words.size() == 1) {
	    _compact_config = true;
	    type = compact_config_str;
	} else if (type == package_str && words.size() == 2
		   && cp_string(words[1], &value))
	    /* OK */;
	else if (type == library_str && words.size() == 2
		 && cp_string(words[1], &value)) {
	    yrequire_library(value);
	    continue;
	} else {
	    lerror("syntax error at requirement");
	    continue;
	}

	if (_lextra)
	    _lextra->require(type, value, _errh);
	_requirements.push_back(type);
	_requirements.push_back(value);
    }
}

void
Lexer::yvar()
{
  if (expect('(')) {
    String requirement = lex_config();
    expect(')');

    Vector<String> args;
    String word;
    cp_argvec(requirement, args);
    for (int i = 0; i < args.size(); i++)
      if (args[i]) {
	String var = cp_shift_spacevec(args[i]);
	const char *s = var.begin();
	if (s != var.end() && *s == '$')
	  for (s++; s != var.end() && (isalnum((unsigned char) *s) || *s == '_'); s++)
	    /* nada */;
	if (var.length() < 2 || s != var.end())
	  lerror("bad %<define%> declaration: not a variable");
	else {
	  var = var.substring(1);
	  _c->define(var, args[i], false, this);
	}
      }
  }
}

bool
Lexer::ystatement(int nested)
{
  Lexeme t = lex();
  switch (t.kind()) {

   case lexIdent:
   case '[':
   case '{':
   case '(':
   case lexArrow:
   case lex2Arrow:
    unlex(t);
    yconnection();
    return true;

   case lexElementclass:
    yelementclass();
    return true;

   case lexRequire:
    yrequire();
    return true;

   case lexDefine:
    yvar();
    return true;

   case ';':
    return true;

   case '}':
   case lex2Bar:
    if (nested != '}')
      goto syntax_error;
    unlex(t);
    return false;

   case ')':
    if (nested != ')')
      goto syntax_error;
    unlex(t);
    return false;

   case lexEOF:
    if (nested)
	lerror("expected %<%c%>", nested);
    return false;

   default:
   syntax_error:
    lerror("syntax error near %<%#s%>", t.string().c_str());
    return true;

  }
}


// COMPLETION

void
Lexer::add_router_connections(int c, const Vector<int> &router_id)
{
  Vector<Port> hfrom;
  expand_connection(_c->_conn[c][1], true, hfrom);
  Vector<Port> hto;
  expand_connection(_c->_conn[c][0], false, hto);
  for (int f = 0; f < hfrom.size(); f++) {
    int eidx = router_id[hfrom[f].idx];
    if (eidx >= 0)
      for (int t = 0; t < hto.size(); t++) {
	int tidx = router_id[hto[t].idx];
	if (tidx >= 0)
	  _c->connect(hfrom[f].idx, hfrom[f].port, hto[t].idx, hto[t].port);
      }
  }
}

void
Lexer::expand_compound_element(int which, VariableEnvironment &ve)
{
  String name = _c->_element_names[which];
  int etype = _c->_elements[which];
  assert(name);

  // deanonymize element name if necessary
  if (name[0] == ';')
    name = _c->_element_names[which] = deanonymize_element_name(name, which);

  // avoid TUNNEL_TYPE
  if (etype == TUNNEL_TYPE)
    return;

  // expand config string
  _c->_element_configurations[which] = cp_expand(_c->_element_configurations[which], ve);

  // exit if not compound
  if (_element_types[etype].factory != compound_element_factory)
    return;
  Compound *c = (Compound *) _element_types[etype].thunk;

  // find right version
  Vector<String> args;
  cp_argvec(_c->_element_configurations[which], args);
  int inputs_used = _c->_element_nports[0][which];
  int outputs_used = _c->_element_nports[1][which];

  int found_type = c->resolve(this, etype, inputs_used, outputs_used, args, _errh, _file.landmark());

  // check for error or non-compound, or expand compound
  if (found_type < 0)
    _c->_elements[which] = ERROR_TYPE;
  else if (_element_types[found_type].factory != compound_element_factory)
    _c->_elements[which] = found_type;
  else {
    Compound *found_comp = (Compound *) _element_types[found_type].thunk;

    VariableEnvironment new_ve(ve.parent_of(found_comp->depth()));
    for (int i = 0; i < found_comp->nformals(); i++)
      new_ve.define(found_comp->scope().name(i), args[i], true);
    for (int i = found_comp->nformals(); i < found_comp->scope().size(); i++)
      new_ve.define(found_comp->scope().name(i), cp_expand(found_comp->scope().value(i), new_ve), true);

    found_comp->expand_into(this, which, new_ve);
  }
}

Router *
Lexer::create_router(Master *master)
{
  Router *router = new Router(_file._big_string, master);
  if (!router)
    return 0;

  // expand compounds
  for (int i = 0; i < _global_scope.size(); i++)
    _c->scope().define(_global_scope.name(i), _global_scope.value(i), true);
  int initial_elements_size = _c->_elements.size();
  for (int i = 0; i < initial_elements_size; i++)
    expand_compound_element(i, _c->scope());

  // add elements to router
  Vector<int> router_id;
  for (int i = 0; i < _c->_elements.size(); i++) {
    int etype = _c->_elements[i];
    if (etype == TUNNEL_TYPE)
      router_id.push_back(-1);
#if CLICK_LINUXMODULE
    else if (_element_types[etype].module && router->add_module_ref(_element_types[etype].module) < 0) {
      _errh->lerror(_c->element_landmark(i), "module for element type %<%s%> unloaded", _element_types[etype].name.c_str());
      router_id.push_back(-1);
    }
#endif
    else if (Element *e = (*_element_types[etype].factory)(_element_types[etype].thunk)) {
      int ei = router->add_element(e, _c->_element_names[i], _c->_element_configurations[i], _c->_element_filenames[i], _c->_element_linenos[i]);
      router_id.push_back(ei);
    } else {
      _errh->lerror(_c->element_landmark(i), "failed to create element %<%s%>", _c->_element_names[i].c_str());
      router_id.push_back(-1);
    }
  }

  // first-level connection expansion
  if (_tunnels.size()) {
    for (const Connection *cp = _c->_conn.begin(); cp != _c->_conn.end(); ++cp)
      for (int isoutput = 0; isoutput < 2; ++isoutput)
	if (router_id[(*cp)[isoutput].idx] < 0)
	  if (TunnelEnd *te = find_tunnel((*cp)[isoutput], isoutput, false))
	    te->other()->_correspond.push_back((*cp)[!isoutput]);
  }

  // expand connections to router
  int pre_expanded_nc = _c->_conn.size();
  for (int i = 0; i < pre_expanded_nc; i++) {
    int fromi = router_id[ _c->_conn[i][1].idx ];
    int toi = router_id[ _c->_conn[i][0].idx ];
    if (fromi < 0 || toi < 0)
      add_router_connections(i, router_id);
  }

  // use router element numbers
  for (Connection *cp = _c->_conn.begin(); cp != _c->_conn.end(); ++cp) {
    (*cp)[0].idx = router_id[(*cp)[0].idx];
    (*cp)[1].idx = router_id[(*cp)[1].idx];
  }

  // sort and add connections to router
  click_qsort(_c->_conn.begin(), _c->_conn.size());
  for (Connection *cp = _c->_conn.begin(); cp != _c->_conn.end(); ++cp)
    if ((*cp)[0].idx >= 0 && (*cp)[1].idx >= 0)
      router->add_connection((*cp)[1].idx, (*cp)[1].port, (*cp)[0].idx, (*cp)[0].port);

  // add requirements to router
  for (int i = 0; i < _requirements.size(); i += 2)
      router->add_requirement(_requirements[i], _requirements[i+1]);

  return router;
}


//
// LEXEREXTRA
//

void
LexerExtra::require(String, String, ErrorHandler *)
{
}


//
// LEXER::TUNNELEND RELATED STUFF
//

Lexer::TunnelEnd *
Lexer::find_tunnel(const Port &h, bool isoutput, bool insert)
{
  // binary search for tunnel
  unsigned l = 0, r = _tunnels.size();
  while (l < r) {
    unsigned m = l + (r - l) / 2;
    if (h.idx < _tunnels[m]->_port.idx)
      r = m;
    else if (h.idx > _tunnels[m]->_port.idx)
      l = m + 1;
    else {
      l = m;
      r = m + 1;
      break;
    }
  }

  // insert space if necessary
  if (l >= r && insert) {
    _tunnels.insert(_tunnels.begin() + l, 0);
    ++r;
  } else if (l >= r)
    return 0;

  // find match
  TunnelEnd *match = 0;
  for (TunnelEnd *te = _tunnels[l]; te; te = te->next())
    if (te->isoutput() == isoutput && te->port().port == h.port)
      return te;
    else if (te->isoutput() == isoutput && te->port().port == 0)
      match = te;

  // add new end if necessary
  if (match && !insert) {
    TunnelEnd *te = new TunnelEnd(h, isoutput, _tunnels[l]);
    _tunnels[l] = te;
    TunnelEnd *ote = find_tunnel(Port(match->other()->port().idx, h.port), !isoutput, true);
    te->pair_with(ote);
    return te;
  } else if (insert) {
    TunnelEnd *te = new TunnelEnd(h, isoutput, _tunnels[l]);
    _tunnels[l] = te;
    return te;
  } else
    return 0;
}

void
Lexer::TunnelEnd::expand(Lexer *lexer, Vector<Router::Port> &into)
{
  if (_expanded == 1)
    return;

  if (_expanded == 0) {
    _expanded = 1;

    // _correspond contains the first cut at corresponding ports
    Vector<Router::Port> connections;
    connections.swap(_correspond);

    // give good errors for unused or nonexistent compound element ports
    if (!connections.size()) {
      Port inh = (_isoutput ? _other->_port : _port);
      Port outh = (_isoutput ? _port : _other->_port);
      String in_name = lexer->element_name(inh.idx);
      String out_name = lexer->element_name(outh.idx);
      if (in_name + "/input" == out_name) {
	const char *message = (_isoutput ? "%<%s%> input %d unused"
			       : "%<%s%> has no input %d");
	lexer->errh()->lerror(lexer->element_landmark(inh.idx), message,
			      in_name.c_str(), inh.port);
      } else if (in_name == out_name + "/output") {
	const char *message = (_isoutput ? "%<%s%> has no output %d"
			       : "%<%s%> output %d unused");
	lexer->errh()->lerror(lexer->element_landmark(outh.idx), message,
			      out_name.c_str(), outh.port);
      } else {
	lexer->errh()->lerror(lexer->element_landmark(_other->_port.idx),
			      "tunnel %<%s -> %s%> %s %d unused",
			      in_name.c_str(), out_name.c_str(),
			      port_names[_isoutput], _port.idx);
      }
    }

    for (int i = 0; i < connections.size(); i++)
      lexer->expand_connection(connections[i], _isoutput, _correspond);

    _expanded = 2;
  }

  for (int i = 0; i < _correspond.size(); i++)
    into.push_back(_correspond[i]);
}

void
Lexer::expand_connection(const Port &this_end, bool is_out, Vector<Port> &into)
{
    if (_c->_elements[this_end.idx] != TUNNEL_TYPE)
	into.push_back(this_end);
    else if (TunnelEnd *dp = find_tunnel(this_end, is_out, false))
	dp->expand(this, into);
    else if (find_tunnel(this_end, !is_out, false))
	_errh->lerror(_c->element_landmark(this_end.idx), "%<%s%> used as %s",
		      element_name(this_end.idx).c_str(), port_names[is_out]);
}

CLICK_ENDDECLS
