#pragma once

#include <cstring>
#include <exception>
#include <string>
#include <Common/DateLUT.h>
#include <Common/DateLUTImpl.h>


/** Stores a calendar date in broken-down form (year, month, day-in-month).
  * Could be initialized from date in text form, like '2011-01-01' or from time_t with rounding to date.
  * Also could be initialized from date in text form like '20110101... (only first 8 symbols are used).
  * Could be implicitly cast to time_t.
  * NOTE: Transforming between time_t and LocalDate is done in local time zone!
  *
  * When local time was shifted backwards (due to daylight saving time or whatever reason)
  *  - then to resolve the ambiguity of transforming to time_t, lowest of two possible values is selected.
  *
  * packed - for memcmp to work naturally (but because m_year is 2 bytes, on little endian, comparison is correct only before year 2047)
  */
class LocalDate
{
private:
    unsigned short m_year; /// NOLINT
    unsigned char m_month;
    unsigned char m_day;

    void init(time_t time, const DateLUTImpl & date_lut)
    {
        const auto & values = date_lut.getValues(time);

        m_year = values.year;
        m_month = values.month;
        m_day = values.day_of_month;
    }

    void init(const char * s, size_t length)
    {
        if (length < 8)
            throw std::runtime_error("Cannot parse LocalDate: " + std::string(s, length));

        m_year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');

        if (s[4] == '-')
        {
            if (length < 10)
                throw std::runtime_error("Cannot parse LocalDate: " + std::string(s, length));
            m_month = (s[5] - '0') * 10 + (s[6] - '0');
            m_day = (s[8] - '0') * 10 + (s[9] - '0');
        }
        else
        {
            m_month = (s[4] -'0') * 10 + (s[5] -'0');
            m_day = (s[6] - '0')* 10 + (s[7] -'0');
        }
    }

public:
    explicit LocalDate(time_t time, const DateLUTImpl & time_zone = DateLUT::instance())
    {
        init(time, time_zone);
    }

    LocalDate(DayNum day_num, const DateLUTImpl & time_zone = DateLUT::instance()) /// NOLINT
    {
        const auto & values = time_zone.getValues(day_num);
        m_year  = values.year;
        m_month = values.month;
        m_day   = values.day_of_month;
    }

    explicit LocalDate(ExtendedDayNum day_num, const DateLUTImpl & time_zone = DateLUT::instance())
    {
        const auto & values = time_zone.getValues(day_num);
        m_year  = values.year;
        m_month = values.month;
        m_day   = values.day_of_month;
    }

    LocalDate(unsigned short year_, unsigned char month_, unsigned char day_) /// NOLINT
        : m_year(year_), m_month(month_), m_day(day_)
    {
    }

    explicit LocalDate(const std::string & s)
    {
        init(s.data(), s.size());
    }

    LocalDate(const char * data, size_t length)
    {
        init(data, length);
    }

    LocalDate() : m_year(0), m_month(0), m_day(0)
    {
    }

    LocalDate(const LocalDate &) noexcept = default;
    LocalDate & operator= (const LocalDate &) noexcept = default;

    DayNum getDayNum(const DateLUTImpl & lut = DateLUT::instance()) const
    {
        return DayNum(lut.makeDayNum(m_year, m_month, m_day).toUnderType());
    }

    ExtendedDayNum getExtenedDayNum(const DateLUTImpl & lut = DateLUT::instance()) const
    {
        return ExtendedDayNum (lut.makeDayNum(m_year, m_month, m_day).toUnderType());
    }

    operator DayNum() const /// NOLINT
    {
        return getDayNum();
    }

    unsigned short year() const { return m_year; } /// NOLINT
    unsigned char month() const { return m_month; }
    unsigned char day() const { return m_day; }

    void year(unsigned short x) { m_year = x; } /// NOLINT
    void month(unsigned char x) { m_month = x; }
    void day(unsigned char x) { m_day = x; }

    bool operator< (const LocalDate & other) const
    {
        return 0 > memcmp(this, &other, sizeof(*this));
    }

    bool operator> (const LocalDate & other) const
    {
        return 0 < memcmp(this, &other, sizeof(*this));
    }

    bool operator<= (const LocalDate & other) const
    {
        return 0 >= memcmp(this, &other, sizeof(*this));
    }

    bool operator>= (const LocalDate & other) const
    {
        return 0 <= memcmp(this, &other, sizeof(*this));
    }

    bool operator== (const LocalDate & other) const
    {
        return 0 == memcmp(this, &other, sizeof(*this));
    }

    bool operator!= (const LocalDate & other) const
    {
        return !(*this == other);
    }
};

static_assert(sizeof(LocalDate) == 4);
