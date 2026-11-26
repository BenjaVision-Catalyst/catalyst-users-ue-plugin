#include "SubSystem/CUGameProfileSession.h"

#include "Engine/LocalPlayer.h"
#include "Misc/Compression.h"
#include "SubSystem/CCSubsystem.h"

namespace
{
TOptional<std::string> ProfileGraphJson(TConstArrayView<uint8> Body)
{
    constexpr int32 HeaderSize = 8;
    constexpr uint32 MaxGraphBytes = 65536;
    if (Body.Num() < 4 || FMemory::Memcmp(Body.GetData(), "ZLIB", 4) != 0)
    {
        return std::string(reinterpret_cast<const char*>(Body.GetData()), Body.Num());
    }
    if (Body.Num() <= HeaderSize)
    {
        return {};
    }

    const uint32 Size = uint32(Body[4]) | (uint32(Body[5]) << 8) |
        (uint32(Body[6]) << 16) | (uint32(Body[7]) << 24);
    if (Size == 0 || Size > MaxGraphBytes)
    {
        return {};
    }
    std::string Json(Size, '\0');
    if (!FCompression::UncompressMemory(NAME_Zlib, Json.data(), Size,
        Body.GetData() + HeaderSize, Body.Num() - HeaderSize))
    {
        return {};
    }
    return Json;
}

class FCUPlayerProfileSource final : public ICFPlayerMagritteSource
{
public:
    explicit FCUPlayerProfileSource(UCUGameProfileSession* InSession) : Session(InSession) {}

    TOptional<FCFMagritteBinding> SignedInUser(const ULocalPlayer& Player) const override
    {
        const auto* Owner = Session.Get();
        return Owner && Owner->GetLocalPlayer() == &Player ? Owner->CurrentUser()
                                                           : TOptional<FCFMagritteBinding>();
    }

    FSimpleMulticastDelegate& OnChanged() override { return Changed; }

private:
    TWeakObjectPtr<UCUGameProfileSession> Session;
    FSimpleMulticastDelegate Changed;
};
}

void UCUGameProfileSession::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    Source = MakeShared<FCUPlayerProfileSource>(this);
}

void UCUGameProfileSession::Deinitialize()
{
    ClearProfile();
    if (auto* Transport = Connect.Get())
    {
        Transport->OnConnectionStateChanged.RemoveDynamic(this, &UCUGameProfileSession::ConnectionChanged);
    }
    Connect.Reset();
    Source.Reset();
    Super::Deinitialize();
}

void UCUGameProfileSession::ClearProfile()
{
    ++LoadSerial;
    if (auto* Transport = Connect.Get(); Transport && ActiveRequest)
    {
        Transport->RetireRequest(ActiveConnection, ActiveRequest, ActiveGeneration);
    }
    ActiveRequest = 0;
    ActiveConnection = NAME_None;
    ActiveGeneration = 0;
    User.Reset();
    if (Source)
    {
        Source->OnChanged().Broadcast();
    }
}

TOptional<FCFMagritteBinding> UCUGameProfileSession::CurrentUser() const
{
    const auto* Transport = Connect.Get();
    if (!Transport || !ActiveGeneration ||
        Transport->GetConnectionGeneration(ActiveConnection) != ActiveGeneration ||
        Transport->GetConnectionState(ActiveConnection) != ECCConnectState::Ready)
    {
        return {};
    }
    return User;
}

void UCUGameProfileSession::ConnectionChanged(FName Connection, ECCConnectState State, int32)
{
    if (Connection == ActiveConnection && State != ECCConnectState::Ready)
    {
        ClearProfile();
    }
}

bool UCUGameProfileSession::LoadCurrentGameProfile(FName Connection, uint64 AuthenticatedGeneration, uint16 Operation,
    TConstArrayView<uint8> Request, const FString& VerifiedUserId)
{
    ClearProfile();
    if (!Source || VerifiedUserId.IsEmpty())
    {
        return false;
    }
    auto* Player = GetLocalPlayer();
    auto* Transport = Player ? UCCSubsystem::Get(Player->GetGameInstance()) : nullptr;
    if (!Transport || !AuthenticatedGeneration ||
        Transport->GetConnectionGeneration(Connection) != AuthenticatedGeneration ||
        Transport->GetConnectionState(Connection) != ECCConnectState::Ready)
    {
        return false;
    }
    if (Connect.Get() != Transport)
    {
        if (auto* Previous = Connect.Get())
        {
            Previous->OnConnectionStateChanged.RemoveDynamic(this, &UCUGameProfileSession::ConnectionChanged);
        }
        Connect = Transport;
        Transport->OnConnectionStateChanged.AddUniqueDynamic(this, &UCUGameProfileSession::ConnectionChanged);
    }
    ActiveConnection = Connection;
    ActiveGeneration = AuthenticatedGeneration;
    if (QueueProfileGraph(Operation, Request, VerifiedUserId))
    {
        return true;
    }
    ClearProfile();
    return false;
}

bool UCUGameProfileSession::RequestProfileUpdate(uint16 Operation, TConstArrayView<uint8> Request)
{
    const auto Current = CurrentUser();
    return Operation && Current && !ActiveRequest
        && QueueProfileGraph(Operation, Request, UTF8_TO_TCHAR(Current->ObjectId().c_str()));
}

bool UCUGameProfileSession::QueueProfileGraph(uint16 Operation, TConstArrayView<uint8> Request, const FString& VerifiedUserId)
{
    auto* Transport = Connect.Get();
    const uint64 Generation = ActiveGeneration;
    const uint64 Serial = LoadSerial;
    const auto Result = Transport->TryRequest(ActiveConnection, Transport, nullptr, Operation, Request,
        FCCConnectReplyCallback::CreateWeakLambda(this, [this, Serial, Generation, VerifiedUserId](const FCCConnectReply& Reply)
        {
            if (LoadSerial != Serial || Reply.Generation != Generation)
            {
                return;
            }
            ActiveRequest = 0;
            if (Reply.Status != 0 || Reply.Body.IsEmpty() || !Connect.IsValid()
                || Connect->GetConnectionGeneration(ActiveConnection) != Generation)
            {
                if (!User && Source)
                {
                    Source->OnChanged().Broadcast();
                }
                return;
            }

            try
            {
                if (const auto Json = ProfileGraphJson(Reply.Body))
                {
                    auto Graph = std::make_shared<magritte::Graph>(magritte::Graph::decode(*Json));
                    auto Candidate = FCFMagritteBinding::Find(Graph, TCHAR_TO_UTF8(*VerifiedUserId));
                    if (Candidate && Candidate->Describe().id == Graph->root)
                    {
                        User = MoveTemp(Candidate);
                        Source->OnChanged().Broadcast();
                        return;
                    }
                }
            }
            catch (const std::exception&)
            {
                // A rejected update keeps the last verified profile available.
            }
            if (!User && Source)
            {
                Source->OnChanged().Broadcast();
            }
        }), 10.0, Generation);
    if (Result.Result != ECCConnectResult::Queued)
    {
        return false;
    }
    ActiveRequest = Result.RequestId;
    return true;
}
