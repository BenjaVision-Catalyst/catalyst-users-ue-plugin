#include "SubSystem/CUGameProfileSession.h"

#include "Engine/LocalPlayer.h"
#include "SubSystem/CCSubsystem.h"

namespace
{
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
    const uint64 Generation = AuthenticatedGeneration;
    const uint64 Serial = LoadSerial;
    const auto Result = Transport->TryRequest(Connection, Transport, nullptr, Operation, Request,
        FCCConnectReplyCallback::CreateWeakLambda(this, [this, Serial, Generation, VerifiedUserId](const FCCConnectReply& Reply)
        {
            if (LoadSerial != Serial || Reply.Generation != Generation)
            {
                return;
            }
            ActiveRequest = 0;
            if (Reply.Status == 0 && !Reply.Body.IsEmpty() &&
                Connect.IsValid() && Connect->GetConnectionGeneration(ActiveConnection) == Generation)
            {
                try
                {
                    auto Graph = std::make_shared<magritte::Graph>(magritte::Graph::decode(
                        std::string(reinterpret_cast<const char*>(Reply.Body.GetData()), Reply.Body.Num())));
                    auto Candidate = FCFMagritteBinding::Find(Graph, TCHAR_TO_UTF8(*VerifiedUserId));
                    if (Candidate && Candidate->Describe().id == Graph->root)
                    {
                        User = MoveTemp(Candidate);
                    }
                }
                catch (const std::exception&)
                {
                    User.Reset();
                }
            }
            Source->OnChanged().Broadcast();
        }), 10.0, Generation);
    if (Result.Result != ECCConnectResult::Queued)
    {
        return false;
    }
    ActiveConnection = Connection;
    ActiveGeneration = Generation;
    ActiveRequest = Result.RequestId;
    return true;
}
