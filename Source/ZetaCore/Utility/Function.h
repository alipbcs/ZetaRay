#pragma once

#include "../App/ZetaRay.h"

namespace ZetaRay::Util
{
    // Ref: https://stackoverflow.com/questions/18633697/fastdelegate-and-lambdas-cant-get-them-to-work-don-clugstons-fastest-possib
    struct Function
    {
        Function() = default;

        template <typename F>
        Function(F&& f)
        {
            static_assert(sizeof(F) <= BUFFER_SIZE, "Memory needed exceeded capture buffer size.");
            static_assert(std::is_move_constructible_v<F>);

            m_lambda = &Get<F>();
            new (&m_buffer) F(ZetaMove(f));
        }

        ~Function()
        {
            if (m_lambda)
            {
                m_lambda->destruct(&m_buffer);
            }
        }

        Function(Function&& other)
            : m_lambda(other.m_lambda)
        {
            other.m_lambda = nullptr;
            memcpy(m_buffer, other.m_buffer, BUFFER_SIZE);
            memset(other.m_buffer, 0, BUFFER_SIZE);
        }

        Function& operator=(Function&& other)
        {
            m_lambda = other.m_lambda;
            other.m_lambda = nullptr;
            memcpy(m_buffer, other.m_buffer, BUFFER_SIZE);
            memset(other.m_buffer, 0, BUFFER_SIZE);

            return *this;
        }

        bool IsSet() const
        {
            return m_lambda != nullptr;
        }

        //template <typename F>
        //void Reset(F&& f)
        //{
        //    static_assert(sizeof(F) <= BUFFER_SIZE, "Memory needed exceede capture buffer size.");
        //    static_assert(std::is_move_constructible_v<F>);

        //    if (m_lambda)
        //        m_lambda->destruct(&m_buffer);

        //    m_lambda = &Get<F>();
        //    new (&m_buffer) F(ZetaMove(f));
        //}

        ZetaInline void Run()
        {
            return m_lambda->call(&m_buffer);
        }

    private:
#if defined(__clang__)
        // Due to [[no_unique_address]] not working, following needs to be larger
        static constexpr int BUFFER_SIZE = 40;
#elif defined(_MSC_VER)
        static constexpr int BUFFER_SIZE = 32;
#endif

        struct LambdaFuncPtrs
        {
            void (*call)(void*);
            void (*destruct)(void*);
        };

        template <typename F>
        static void Call(void* f)
        {
            (*reinterpret_cast<F*>(f))();
        }

        template <typename F>
        static void Destruct(void* f)
        {
            reinterpret_cast<F*>(f)->~F();
        }

        template <typename F>
        const LambdaFuncPtrs& Get()
        {
            static const LambdaFuncPtrs lambda = { &Call<F>, &Destruct<F> };
            return lambda;
        }

        const LambdaFuncPtrs* m_lambda = nullptr;
        alignas(alignof(std::max_align_t)) uint8_t m_buffer[BUFFER_SIZE];
    };
}