/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-marshal-validate.c Validation routines for marshaled data
 *
 * Copyright (C) 2005 Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-marshal-validate.h"
#include "dbus-marshal-recursive.h"

/**
 * @addtogroup DBusMarshal
 *
 * @{
 */

/**
 * Verifies that the range of type_str from type_pos to type_end is a
 * valid signature.  If this function returns #TRUE, it will be safe
 * to iterate over the signature with a types-only #DBusTypeReader.
 * The range passed in should NOT include the terminating
 * nul/DBUS_TYPE_INVALID.
 *
 * @param type_str the string
 * @param type_pos where the typecodes start
 * @param len length of typecodes
 * @returns #DBUS_VALID if valid, reason why invalid otherwise
 */
DBusValidity
_dbus_validate_signature_with_reason (const DBusString *type_str,
                                      int               type_pos,
                                      int               len)
{
  const unsigned char *p;
  const unsigned char *end;
  int last;
  int struct_depth;
  int array_depth;

  _dbus_assert (type_str != NULL);
  _dbus_assert (type_pos < _DBUS_INT_MAX - len);
  _dbus_assert (len >= 0);
  _dbus_assert (type_pos >= 0);

  if (len > DBUS_MAXIMUM_SIGNATURE_LENGTH)
    return DBUS_INVALID_SIGNATURE_TOO_LONG;

  p = _dbus_string_get_const_data_len (type_str, type_pos, 0);
  end = _dbus_string_get_const_data_len (type_str, type_pos + len, 0);
  struct_depth = 0;
  array_depth = 0;
  last = DBUS_TYPE_INVALID;

  while (p != end)
    {
      switch (*p)
        {
        case DBUS_TYPE_BYTE:
        case DBUS_TYPE_BOOLEAN:
        case DBUS_TYPE_INT32:
        case DBUS_TYPE_UINT32:
        case DBUS_TYPE_INT64:
        case DBUS_TYPE_UINT64:
        case DBUS_TYPE_DOUBLE:
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
        case DBUS_TYPE_VARIANT:
          break;

        case DBUS_TYPE_ARRAY:
          array_depth += 1;
          if (array_depth > DBUS_MAXIMUM_TYPE_RECURSION_DEPTH)
            return DBUS_INVALID_EXCEEDED_MAXIMUM_ARRAY_RECURSION;
          break;

        case DBUS_STRUCT_BEGIN_CHAR:
          struct_depth += 1;

          if (struct_depth > DBUS_MAXIMUM_TYPE_RECURSION_DEPTH)
            return DBUS_INVALID_EXCEEDED_MAXIMUM_STRUCT_RECURSION;
          break;

        case DBUS_STRUCT_END_CHAR:
          if (struct_depth == 0)
            return DBUS_INVALID_STRUCT_ENDED_BUT_NOT_STARTED;

          if (last == DBUS_STRUCT_BEGIN_CHAR)
            return DBUS_INVALID_STRUCT_HAS_NO_FIELDS;

          struct_depth -= 1;
          break;

        case DBUS_TYPE_STRUCT: /* doesn't appear in signatures */
        default:
          return DBUS_INVALID_UNKNOWN_TYPECODE;
        }

      if (*p != DBUS_TYPE_ARRAY)
        array_depth = 0;

      last = *p;
      ++p;
    }

  if (array_depth > 0)
    return DBUS_INVALID_MISSING_ARRAY_ELEMENT_TYPE;

  if (struct_depth > 0)
    return DBUS_INVALID_STRUCT_STARTED_BUT_NOT_ENDED;

  return DBUS_VALID;
}

static DBusValidity
validate_body_helper (DBusTypeReader       *reader,
                      int                   byte_order,
                      dbus_bool_t           walk_reader_to_end,
                      const unsigned char  *p,
                      const unsigned char  *end,
                      const unsigned char **new_p)
{
  int current_type;

  while ((current_type = _dbus_type_reader_get_current_type (reader)) != DBUS_TYPE_INVALID)
    {
      const unsigned char *a;
      int alignment;

      _dbus_verbose ("   validating value of type %s type reader %p type_pos %d p %p end %p %d remain\n",
                     _dbus_type_to_string (current_type), reader, reader->type_pos, p, end,
                     (int) (end - p));

      /* Guarantee that p has one byte to look at */
      if (p == end)
        return DBUS_INVALID_NOT_ENOUGH_DATA;

      switch (current_type)
        {
        case DBUS_TYPE_BYTE:
          ++p;
          break;

        case DBUS_TYPE_BOOLEAN:
        case DBUS_TYPE_INT32:
        case DBUS_TYPE_UINT32:
        case DBUS_TYPE_INT64:
        case DBUS_TYPE_UINT64:
        case DBUS_TYPE_DOUBLE:
          alignment = _dbus_type_get_alignment (current_type);
          a = _DBUS_ALIGN_ADDRESS (p, alignment);
          if (a >= end)
            return DBUS_INVALID_NOT_ENOUGH_DATA;
          while (p != a)
            {
              if (*p != '\0')
                return DBUS_INVALID_ALIGNMENT_PADDING_NOT_NUL;
              ++p;
            }
          
          if (current_type == DBUS_TYPE_BOOLEAN)
            {
              dbus_uint32_t v = _dbus_unpack_uint32 (byte_order,
                                                     p);
              if (!(v == 0 || v == 1))
                return DBUS_INVALID_BOOLEAN_NOT_ZERO_OR_ONE;
            }
          
          p += alignment;
          break;

        case DBUS_TYPE_ARRAY:
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
          {
            dbus_uint32_t claimed_len;

            a = _DBUS_ALIGN_ADDRESS (p, 4);
            if (a + 4 > end)
              return DBUS_INVALID_NOT_ENOUGH_DATA;
            while (p != a)
              {
                if (*p != '\0')
                  return DBUS_INVALID_ALIGNMENT_PADDING_NOT_NUL;
                ++p;
              }

            claimed_len = _dbus_unpack_uint32 (byte_order, p);
            p += 4;

            /* p may now be == end */
            _dbus_assert (p <= end);
            
            if (current_type == DBUS_TYPE_ARRAY)
              {
                int array_elem_type = _dbus_type_reader_get_element_type (reader);
                alignment = _dbus_type_get_alignment (array_elem_type);
                p = _DBUS_ALIGN_ADDRESS (p, alignment);
              }

            if (claimed_len > (unsigned long) (end - p))
              return DBUS_INVALID_STRING_LENGTH_OUT_OF_BOUNDS;

            if (current_type == DBUS_TYPE_OBJECT_PATH)
              {
                DBusString str;
                _dbus_string_init_const_len (&str, p, claimed_len);
                if (!_dbus_validate_path (&str, 0,
                                          _dbus_string_get_length (&str)))
                  return DBUS_INVALID_BAD_PATH;

                p += claimed_len;
              }
            else if (current_type == DBUS_TYPE_STRING)
              {
                DBusString str;
                _dbus_string_init_const_len (&str, p, claimed_len);
                if (!_dbus_string_validate_utf8 (&str, 0,
                                                 _dbus_string_get_length (&str)))
                  return DBUS_INVALID_BAD_UTF8_IN_STRING;

                p += claimed_len;
              }
            else if (current_type == DBUS_TYPE_ARRAY && claimed_len > 0)
              {
                DBusTypeReader sub;
                DBusValidity validity;
                const unsigned char *array_end;

                /* Remember that the reader is types only, so we can't
                 * use it to iterate over elements. It stays the same
                 * for all elements.
                 */
                _dbus_type_reader_recurse (reader, &sub);

                array_end = p + claimed_len;

                while (p < array_end)
                  {
                    validity = validate_body_helper (&sub, byte_order, FALSE, p, end, &p);
                    if (validity != DBUS_VALID)
                      return validity;
                  }

                if (p != array_end)
                  return DBUS_INVALID_ARRAY_LENGTH_INCORRECT;
              }

            /* check nul termination */
            if (current_type != DBUS_TYPE_ARRAY)
              {
                if (p == end)
                  return DBUS_INVALID_NOT_ENOUGH_DATA;

                if (*p != '\0')
                  return DBUS_INVALID_STRING_MISSING_NUL;
                ++p;
              }
          }
          break;

        case DBUS_TYPE_SIGNATURE:
          {
            dbus_uint32_t claimed_len;
            DBusString str;

            claimed_len = *p;
            ++p;

            /* 1 is for nul termination */
            if (claimed_len + 1 > (unsigned long) (end - p))
              return DBUS_INVALID_SIGNATURE_LENGTH_OUT_OF_BOUNDS;

            _dbus_string_init_const_len (&str, p, claimed_len);
            if (!_dbus_validate_signature (&str, 0,
                                           _dbus_string_get_length (&str)))
              return DBUS_INVALID_BAD_SIGNATURE;

            p += claimed_len;

            _dbus_assert (p < end);
            if (*p != DBUS_TYPE_INVALID)
              return DBUS_INVALID_SIGNATURE_MISSING_NUL;

            ++p;

            _dbus_verbose ("p = %p end = %p claimed_len %u\n", p, end, claimed_len);
          }
          break;

        case DBUS_TYPE_VARIANT:
          {
            /* 1 byte sig len, sig typecodes, align to
             * contained-type-boundary, values.
             */

            /* In addition to normal signature validation, we need to be sure
             * the signature contains only a single (possibly container) type.
             */
            dbus_uint32_t claimed_len;
            DBusString sig;
            DBusTypeReader sub;
            DBusValidity validity;
            int contained_alignment;

            claimed_len = *p;
            ++p;

            /* + 1 for nul */
            if (claimed_len + 1 > (unsigned long) (end - p))
              return DBUS_INVALID_VARIANT_SIGNATURE_LENGTH_OUT_OF_BOUNDS;

            _dbus_string_init_const_len (&sig, p, claimed_len);
            if (!_dbus_validate_signature (&sig, 0,
                                           _dbus_string_get_length (&sig)))
              return DBUS_INVALID_VARIANT_SIGNATURE_BAD;

            p += claimed_len;

            if (*p != DBUS_TYPE_INVALID)
              return DBUS_INVALID_VARIANT_SIGNATURE_MISSING_NUL;
            ++p;

            contained_alignment = _dbus_type_get_alignment (_dbus_first_type_in_signature (&sig, 0));
            
            a = _DBUS_ALIGN_ADDRESS (p, contained_alignment);
            if (a > end)
              return DBUS_INVALID_NOT_ENOUGH_DATA;
            while (p != a)
              {
                if (*p != '\0')
                  return DBUS_INVALID_ALIGNMENT_PADDING_NOT_NUL;
                ++p;
              }

            _dbus_type_reader_init_types_only (&sub, &sig, 0);

            if (_dbus_type_reader_get_current_type (&sub) == DBUS_TYPE_INVALID)
              return DBUS_INVALID_VARIANT_SIGNATURE_EMPTY;

            validity = validate_body_helper (&sub, byte_order, FALSE, p, end, &p);
            if (validity != DBUS_VALID)
              return validity;

            if (_dbus_type_reader_next (&sub))
              return DBUS_INVALID_VARIANT_SIGNATURE_SPECIFIES_MULTIPLE_VALUES;

            _dbus_assert (_dbus_type_reader_get_current_type (&sub) == DBUS_TYPE_INVALID);
          }
          break;

        case DBUS_TYPE_STRUCT:
          {
            DBusTypeReader sub;
            DBusValidity validity;

            a = _DBUS_ALIGN_ADDRESS (p, 8);
            if (a > end)
              return DBUS_INVALID_NOT_ENOUGH_DATA;
            while (p != a)
              {
                if (*p != '\0')
                  return DBUS_INVALID_ALIGNMENT_PADDING_NOT_NUL;
                ++p;
              }

            _dbus_type_reader_recurse (reader, &sub);

            validity = validate_body_helper (&sub, byte_order, TRUE, p, end, &p);
            if (validity != DBUS_VALID)
              return validity;
          }
          break;

        default:
          _dbus_assert_not_reached ("invalid typecode in supposedly-validated signature");
          break;
        }

      _dbus_verbose ("   validated value of type %s type reader %p type_pos %d p %p end %p %d remain\n",
                     _dbus_type_to_string (current_type), reader, reader->type_pos, p, end,
                     (int) (end - p));

      if (p > end)
        {
          _dbus_verbose ("not enough data!!! p = %p end = %p end-p = %d\n",
                         p, end, (int) (end - p));
          return DBUS_INVALID_NOT_ENOUGH_DATA;
        }

      if (walk_reader_to_end)
        _dbus_type_reader_next (reader);
      else
        break;
    }

  if (new_p)
    *new_p = p;

  return DBUS_VALID;
}

/**
 * Verifies that the range of value_str from value_pos to value_end is
 * a legitimate value of type expected_signature.  If this function
 * returns #TRUE, it will be safe to iterate over the values with
 * #DBusTypeReader. The signature is assumed to be already valid.
 *
 * If bytes_remaining is not #NULL, then leftover bytes will be stored
 * there and #DBUS_VALID returned. If it is #NULL, then
 * #DBUS_INVALID_TOO_MUCH_DATA will be returned if bytes are left
 * over.
 *
 * @param expected_signature the expected types in the value_str
 * @param expected_signature_start where in expected_signature is the signature
 * @param byte_order the byte order
 * @param bytes_remaining place to store leftover bytes
 * @param value_str the string containing the body
 * @param value_pos where the values start
 * @param len length of values after value_pos
 * @returns #DBUS_VALID if valid, reason why invalid otherwise
 */
DBusValidity
_dbus_validate_body_with_reason (const DBusString *expected_signature,
                                 int               expected_signature_start,
                                 int               byte_order,
                                 int              *bytes_remaining,
                                 const DBusString *value_str,
                                 int               value_pos,
                                 int               len)
{
  DBusTypeReader reader;
  const unsigned char *p;
  const unsigned char *end;
  DBusValidity validity;

  _dbus_assert (len >= 0);
  _dbus_assert (value_pos >= 0);
  _dbus_assert (value_pos <= _dbus_string_get_length (value_str) - len);

  _dbus_verbose ("validating body from pos %d len %d sig '%s'\n",
                 value_pos, len, _dbus_string_get_const_data_len (expected_signature,
                                                                  expected_signature_start,
                                                                  0));

  _dbus_type_reader_init_types_only (&reader,
                                     expected_signature, expected_signature_start);

  p = _dbus_string_get_const_data_len (value_str, value_pos, len);
  end = p + len;

  validity = validate_body_helper (&reader, byte_order, TRUE, p, end, &p);
  if (validity != DBUS_VALID)
    return validity;
  
  if (bytes_remaining)
    {
      *bytes_remaining = end - p;
      return DBUS_VALID;
    }
  else if (p < end)
    return DBUS_INVALID_TOO_MUCH_DATA;
  else
    {
      _dbus_assert (p == end);
      return DBUS_VALID;
    }
}

/**
 * Determine wether the given charater is valid as the first charater
 * in a name.
 */
#define VALID_INITIAL_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
    ((c) == '_') )

/**
 * Determine wether the given charater is valid as a second or later
 * character in a name
 */
#define VALID_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
    ((c) == '_') )

/**
 * Checks that the given range of the string is a valid object path
 * name in the D-BUS protocol. Part of the validation ensures that
 * the object path contains only ASCII.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 *
 * @todo change spec to disallow more things, such as spaces in the
 * path name
 *
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is a valid name
 */
dbus_bool_t
_dbus_validate_path (const DBusString  *str,
                     int                start,
                     int                len)
{
  const unsigned char *s;
  const unsigned char *end;
  const unsigned char *last_slash;

  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= _dbus_string_get_length (str));

  if (len > _dbus_string_get_length (str) - start)
    return FALSE;

  if (len == 0)
    return FALSE;

  s = _dbus_string_get_const_data (str) + start;
  end = s + len;

  if (*s != '/')
    return FALSE;
  last_slash = s;
  ++s;

  while (s != end)
    {
      if (*s == '/')
        {
          if ((s - last_slash) < 2)
            return FALSE; /* no empty path components allowed */

          last_slash = s;
        }
      else
        {
          if (_DBUS_UNLIKELY (!VALID_NAME_CHARACTER (*s)))
            return FALSE;
        }

      ++s;
    }

  if ((end - last_slash) < 2 &&
      len > 1)
    return FALSE; /* trailing slash not allowed unless the string is "/" */

  return TRUE;
}

/**
 * Checks that the given range of the string is a valid interface name
 * in the D-BUS protocol. This includes a length restriction and an
 * ASCII subset, see the specification.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 *
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is a valid name
 */
dbus_bool_t
_dbus_validate_interface (const DBusString  *str,
                          int                start,
                          int                len)
{
  const unsigned char *s;
  const unsigned char *end;
  const unsigned char *iface;
  const unsigned char *last_dot;

  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= _dbus_string_get_length (str));

  if (len > _dbus_string_get_length (str) - start)
    return FALSE;

  if (len > DBUS_MAXIMUM_NAME_LENGTH)
    return FALSE;

  if (len == 0)
    return FALSE;

  last_dot = NULL;
  iface = _dbus_string_get_const_data (str) + start;
  end = iface + len;
  s = iface;

  /* check special cases of first char so it doesn't have to be done
   * in the loop. Note we know len > 0
   */
  if (_DBUS_UNLIKELY (*s == '.')) /* disallow starting with a . */
    return FALSE;
  else if (_DBUS_UNLIKELY (!VALID_INITIAL_NAME_CHARACTER (*s)))
    return FALSE;
  else
    ++s;

  while (s != end)
    {
      if (*s == '.')
        {
          if (_DBUS_UNLIKELY ((s + 1) == end))
            return FALSE;
          else if (_DBUS_UNLIKELY (!VALID_INITIAL_NAME_CHARACTER (*(s + 1))))
            return FALSE;
          last_dot = s;
          ++s; /* we just validated the next char, so skip two */
        }
      else if (_DBUS_UNLIKELY (!VALID_NAME_CHARACTER (*s)))
        {
          return FALSE;
        }

      ++s;
    }

  if (_DBUS_UNLIKELY (last_dot == NULL))
    return FALSE;

  return TRUE;
}

/**
 * Checks that the given range of the string is a valid member name
 * in the D-BUS protocol. This includes a length restriction, etc.,
 * see the specification.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 *
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is a valid name
 */
dbus_bool_t
_dbus_validate_member (const DBusString  *str,
                       int                start,
                       int                len)
{
  const unsigned char *s;
  const unsigned char *end;
  const unsigned char *member;

  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= _dbus_string_get_length (str));

  if (len > _dbus_string_get_length (str) - start)
    return FALSE;

  if (len > DBUS_MAXIMUM_NAME_LENGTH)
    return FALSE;

  if (len == 0)
    return FALSE;

  member = _dbus_string_get_const_data (str) + start;
  end = member + len;
  s = member;

  /* check special cases of first char so it doesn't have to be done
   * in the loop. Note we know len > 0
   */

  if (_DBUS_UNLIKELY (!VALID_INITIAL_NAME_CHARACTER (*s)))
    return FALSE;
  else
    ++s;

  while (s != end)
    {
      if (_DBUS_UNLIKELY (!VALID_NAME_CHARACTER (*s)))
        {
          return FALSE;
        }

      ++s;
    }

  return TRUE;
}

/**
 * Checks that the given range of the string is a valid error name
 * in the D-BUS protocol. This includes a length restriction, etc.,
 * see the specification.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 *
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is a valid name
 */
dbus_bool_t
_dbus_validate_error_name (const DBusString  *str,
                           int                start,
                           int                len)
{
  /* Same restrictions as interface name at the moment */
  return _dbus_validate_interface (str, start, len);
}

/* This assumes the first char exists and is ':' */
static dbus_bool_t
_dbus_validate_unique_name (const DBusString  *str,
                            int                start,
                            int                len)
{
  const unsigned char *s;
  const unsigned char *end;
  const unsigned char *name;

  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= _dbus_string_get_length (str));

  if (len > _dbus_string_get_length (str) - start)
    return FALSE;

  if (len > DBUS_MAXIMUM_NAME_LENGTH)
    return FALSE;

  _dbus_assert (len > 0);

  name = _dbus_string_get_const_data (str) + start;
  end = name + len;
  _dbus_assert (*name == ':');
  s = name + 1;

  while (s != end)
    {
      if (*s == '.')
        {
          if (_DBUS_UNLIKELY ((s + 1) == end))
            return FALSE;
          if (_DBUS_UNLIKELY (!VALID_NAME_CHARACTER (*(s + 1))))
            return FALSE;
          ++s; /* we just validated the next char, so skip two */
        }
      else if (_DBUS_UNLIKELY (!VALID_NAME_CHARACTER (*s)))
        {
          return FALSE;
        }

      ++s;
    }

  return TRUE;
}

/**
 * Checks that the given range of the string is a valid bus name in
 * the D-BUS protocol. This includes a length restriction, etc., see
 * the specification.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 *
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is a valid name
 */
dbus_bool_t
_dbus_validate_bus_name (const DBusString  *str,
                         int                start,
                         int                len)
{
  if (_DBUS_UNLIKELY (len == 0))
    return FALSE;
  if (_dbus_string_get_byte (str, start) == ':')
    return _dbus_validate_unique_name (str, start, len);
  else
    return _dbus_validate_interface (str, start, len);
}

/**
 * Checks that the given range of the string is a valid message type
 * signature in the D-BUS protocol.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 *
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is a valid signature
 */
dbus_bool_t
_dbus_validate_signature (const DBusString  *str,
                          int                start,
                          int                len)
{
  _dbus_assert (start >= 0);
  _dbus_assert (start <= _dbus_string_get_length (str));
  _dbus_assert (len >= 0);

  if (len > _dbus_string_get_length (str) - start)
    return FALSE;

  return _dbus_validate_signature_with_reason (str, start, len) == DBUS_VALID;
}

/** define _dbus_check_is_valid_path() */
DEFINE_DBUS_NAME_CHECK(path);
/** define _dbus_check_is_valid_interface() */
DEFINE_DBUS_NAME_CHECK(interface);
/** define _dbus_check_is_valid_member() */
DEFINE_DBUS_NAME_CHECK(member);
/** define _dbus_check_is_valid_error_name() */
DEFINE_DBUS_NAME_CHECK(error_name);
/** define _dbus_check_is_valid_bus_name() */
DEFINE_DBUS_NAME_CHECK(bus_name);
/** define _dbus_check_is_valid_signature() */
DEFINE_DBUS_NAME_CHECK(signature);

/** @} */

/* tests in dbus-marshal-validate-util.c */
